/* Library index, cache and progress — built from a small fake drive. */
#include "../../core/db.h"
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "library.h"

static const char SRT[] = "1\r\n00:00:01,000 --> 00:00:02,500\r\nHej!\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000\r\n<i>Bra</i>\r\n";

static void put(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static void puts_file(const char *root, const char *rel, const char *text) { put(root, rel, text, strlen(text)); }

static bool fixture(const char *name, const char *root, const char *rel) {
    char src[256];
    snprintf(src, sizeof src, "build/fixtures/%s", name);
    Buf b = {0};
    bool ok = file_read_all(src, &b, 16u << 20);
    if (ok) put(root, rel, b.p, b.len);
    buf_free(&b);
    return ok;
}

/* A drive like the household's: movies, a series with seasons and extras,
 * home videos, subtitles, posters and the usual junk. */
static const char *make_library(char *root, size_t cap) {
    str_copy(root, cap, test_tmpdir());
    puts_file(root, "Movies/Inception (2010)/Inception.2010.1080p.BluRay.x264.mkv", "not really a movie");
    puts_file(root, "Movies/Inception (2010)/poster.jpg", "jpg");
    puts_file(root, "Movies/Inception (2010)/Inception-trailer.mp4", "trailer");
    if (!fixture("flower.mp4", root, "Movies/Flower (2020)/Flower.mp4")) puts_file(root, "Movies/Flower (2020)/Flower.mp4", "x");
    puts_file(root, "Movies/Flower (2020)/Flower.sv.srt", SRT);
    puts_file(root, "Movies/Flower (2020)/Flower.en.srt", SRT);
    puts_file(root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", "e1");
    puts_file(root, "Series/Garden Show/Season 1/Garden.Show.S01E02E03.mp4", "e23");
    puts_file(root, "Series/Garden Show/Season 2/Garden.Show.S02E01.mkv", "e4");
    puts_file(root, "Series/Garden Show/poster.png", "png");
    puts_file(root, "Series/Garden Show/Extras/Behind the scenes.mp4", "bts");
    puts_file(root, "Home Videos/Midsommar 2024/IMG_1234.mov", "home");
    puts_file(root, "$RECYCLE.BIN/deleted.mp4", "gone");
    puts_file(root, ".hidden/secret.mp4", "shh");
    puts_file(root, "notes.txt", "hello");
    return root;
}

static const MediaFile *by_rel(const char *rel) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_eq(ix->files[i].rel, rel)) return &ix->files[i];
    return NULL;
}

/* ---- ripped DVDs: tiny Matroska files that the probe reads a duration from ---- */

static size_t ebml(uint8_t *out, uint32_t id, const void *data, size_t len) {
    size_t n = 0;
    for (int s = 24; s >= 0; s -= 8)
        if (id >> s) out[n++] = (uint8_t)(id >> s);
    out[n++] = (uint8_t)(0x80 | len); /* every element here is < 127 bytes */
    memcpy(out + n, data, len);
    return n + len;
}

/* `cut`: only the start of a big file, as uploads keep of a play-all - the Segment claims 7 GB
   and a Cluster is cut off after a few bytes */
static void put_mkv_ex(const char *root, const char *rel, double minutes, bool cut) {
    uint8_t a[64], b[64], info[64], tr[64], te[64], seg[192], out[256];
    size_t n;
    double ms = minutes * 60000.0;
    uint64_t bits;
    memcpy(&bits, &ms, 8);
    uint8_t be[8];
    for (int i = 0; i < 8; i++) be[i] = (uint8_t)(bits >> (56 - 8 * i));
    n = ebml(a, 0x2AD7B1, "\x0F\x42\x40", 3); /* TimecodeScale 1 ms */
    n += ebml(a + n, 0x4489, be, 8);         /* Duration */
    size_t ni = ebml(info, 0x1549A966, a, n);
    n = ebml(b, 0xB0, "\x02\xD0", 2); /* 720 x 480 */
    n += ebml(b + n, 0xBA, "\x01\xE0", 2);
    size_t nv = ebml(tr, 0xE0, b, n);
    n = ebml(a, 0x83, "\x01", 1); /* video */
    n += ebml(a + n, 0x86, "V_MPEG2", 7);
    memcpy(a + n, tr, nv);
    size_t nt = ebml(te, 0xAE, a, n + nv);
    size_t ntr = ebml(tr, 0x1654AE6B, te, nt);
    memcpy(seg, info, ni);
    memcpy(seg + ni, tr, ntr);
    n = ebml(out, 0x1A45DFA3, "\x42\x82\x88matroska", 11);
    if (!cut) n += ebml(out + n, 0x18538067, seg, ni + ntr);
    else {
        memcpy(out + n, "\x18\x53\x80\x67\x01\x00\x00\x01\xA1\x3B\x86\x00", 12); /* Segment, 7e9 bytes */
        memcpy(out + n + 12, seg, ni + ntr);
        n += 12 + ni + ntr;
        memcpy(out + n, "\x1F\x43\xB6\x75\x88\x81\x00", 7); /* a Cluster of 8 bytes, cut after 2 */
        n += 7;
    }
    put(root, rel, out, n);
}

static void put_mkv(const char *root, const char *rel, double minutes) { put_mkv_ex(root, rel, minutes, false); }

typedef struct {
    const char *name;
    double min;
} Title;

static void put_disc(const char *root, const char *dir, const Title *t, int n) {
    for (int i = 0; i < n; i++) {
        char rel[400];
        snprintf(rel, sizeof rel, "%s/%s", dir, t[i].name);
        put_mkv(root, rel, t[i].min);
    }
}

/* The household's real "70's show" folder, abridged: MakeMKV DVD seasons with a play-all title on
 * every disc and bonus clips, beside scene-named seasons. `show` is the show folder below the root
 * ("" when the root is the show folder itself). Durations are the real ones. */
static void make_rips(const char *root, const char *show) {
    char d[300];
    static const Title s1d1[] = {{"B1_t01.mkv", 22.6}, {"B2_t02.mkv", 22.6}, {"B3_t03.mkv", 22.6}, {"B4_t04.mkv", 22.6},
                                 {"B5_t05.mkv", 22.6}, {"B6_t06.mkv", 22.6}, {"B7_t07.mkv", 22.6}, {"B8_t00.mkv", 158.1}};
    static const Title s1d4[] = {{"B1_t01.mkv", 22.6}, {"B2_t02.mkv", 22.6}, {"B3_t03.mkv", 22.6}, {"B4_t04.mkv", 22.6},
                                 {"B5_t05.mkv", 22.6}, {"B6_t06.mkv", 22.6}, {"B7_t00.mkv", 135.5},
                                 {"C1_t07.mkv", 18.1}, {"C2_t08.mkv", 3.8},  {"C3_t09.mkv", 2.4}};
    /* the 23.3-minute t07 is not part of the play-all: a bonus, however long */
    static const Title s3d1[] = {{"B1_t00.mkv", 133.1}, {"C3_t01.mkv", 22.1}, {"D3_t02.mkv", 22.2}, {"E3_t03.mkv", 22.2},
                                 {"I2_t04.mkv", 22.1},  {"G3_t05.mkv", 22.2}, {"I4_t06.mkv", 22.3}, {"I1_t07.mkv", 23.3}};
    static const Title s6d1[] = {{"C1_t00.mkv", 145.8}, {"D1_t01.mkv", 21.1}, {"E1_t02.mkv", 20.7}, {"F1_t03.mkv", 21.1},
                                 {"G1_t04.mkv", 20.8},  {"H1_t05.mkv", 20.5}, {"I1_t06.mkv", 20.8}, {"J1_t07.mkv", 20.8}};
    static const Title s6d4[] = {{"C4_t08.mkv", 4.3},  {"D1_t00.mkv", 83.7}, {"E1_t01.mkv", 20.6}, {"F1_t03.mkv", 21.1},
                                 {"G1_t04.mkv", 21.1}, {"H3_t02.mkv", 21.0}, {"H4_t05.mkv", 6.7},  {"H6_t06.mkv", 6.4},
                                 {"H7_t07.mkv", 6.0}};
    const char *sep = show[0] ? "/" : "";
    snprintf(d, sizeof d, "%s%sseason 1 70's show/Disc - 1", show, sep);
    put_disc(root, d, s1d1, countof(s1d1));
    snprintf(d, sizeof d, "%s%sseason 1 70's show/Disc - 4", show, sep);
    put_disc(root, d, s1d4, countof(s1d4));
    snprintf(d, sizeof d, "%s%sseason 3 70's show/Disc - 1", show, sep);
    put_disc(root, d, s3d1, countof(s3d1));
    snprintf(d, sizeof d, "%s%sseason 6 70's show/Disc - 1", show, sep);
    put_disc(root, d, s6d1, countof(s6d1));
    snprintf(d, sizeof d, "%s%sseason 6 70's show/Disc - 4", show, sep);
    put_disc(root, d, s6d4, countof(s6d4));
    for (int e = 1; e <= 2; e++) {
        snprintf(d, sizeof d, "%s%sseason 4 70's show/That.70s.Show.S04E%02d.720p.BluRay.ReEnc-DeeJayAhmed.mkv", show, sep,
                 e);
        put_mkv(root, d, 21.2);
    }
    snprintf(d, sizeof d, "%s%sposter.jpg", show, sep);
    if (show[0]) puts_file(root, d, "jpg");
}

/* "S01E08" of the one series -> its file name (without folders) */
static const char *ep_file(int season, int episode) {
    const MediaSeries *s = &media_lib()->series[0];
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (e->name.season == season && e->name.episode == episode) return strrchr(e->rel, '/') + 1;
    }
    return "";
}

static int extras_of(int season, char role) {
    const MediaIndex *ix = media_lib();
    int n = 0;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        n += x->name.season == season && x->role == role;
    }
    return n;
}

static void check_rips(void) {
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nseries, 1); /* disc seasons and scene-named seasons: one show */
    const MediaSeries *s = &ix->series[0];
    CHECK_STR(s->title, "70's Show");
    CHECK_INT(s->nseasons, 4);
    CHECK_INT(s->count, 13 + 6 + 2 + 11);
    CHECK_INT(ix->nepisodes, 32);
    CHECK_INT(ix->nrecent, 32); /* play-alls and clips stay out of every list ... */
    CHECK_INT(ix->nmovies + ix->nothers, 0);
    CHECK_INT(ix->nextras, 13); /* ... but are kept as the series' extras */
    CHECK_INT(extras_of(1, MF_PLAYALL), 2);
    CHECK_INT(extras_of(1, MF_BONUS), 3); /* 18.1 min (overruns the play-all), 3.8 and 2.4 min */
    CHECK_INT(extras_of(3, MF_PLAYALL), 1);
    CHECK_INT(extras_of(3, MF_BONUS), 1); /* the 23.3-minute t07 */
    CHECK_INT(extras_of(6, MF_PLAYALL), 2);
    CHECK_INT(extras_of(6, MF_BONUS), 4);
    /* disc order, then MakeMKV title order — whatever the file names' letters say */
    CHECK_STR(ep_file(1, 1), "B1_t01.mkv");
    CHECK_STR(ep_file(1, 7), "B7_t07.mkv");
    CHECK_STR(ep_file(1, 8), "B1_t01.mkv"); /* disc 4 */
    CHECK_STR(ep_file(1, 13), "B6_t06.mkv");
    CHECK_STR(ep_file(3, 3), "E3_t03.mkv"); /* "E3" is not episode 3's marker, t03 is */
    CHECK_STR(ep_file(3, 4), "I2_t04.mkv");
    CHECK_STR(ep_file(3, 6), "I4_t06.mkv");
    CHECK_STR(ep_file(3, 7), "");
    CHECK_STR(ep_file(4, 2), "That.70s.Show.S04E02.720p.BluRay.ReEnc-DeeJayAhmed.mkv");
    CHECK_STR(ep_file(6, 8), "E1_t01.mkv");
    CHECK_STR(ep_file(6, 9), "H3_t02.mkv");
    CHECK_STR(ep_file(6, 11), "G1_t04.mkv");
    CHECK_STR(ep_file(6, 12), "");
    const MediaFile *e = media_episode(s, 0);
    CHECK(e->name.disc == 1 && e->name.unnumbered);
    CHECK(e->probe.duration_ms == 1356000 && str_eq(e->probe.vcodec, "mpeg2"));
    CHECK(media_neighbour(media_episode(s, 12), +1) == media_episode(s, 13)); /* S1 -> S3 */
    /* an extra is a playable video with a name that says what it is */
    const MediaFile *x = &ix->files[ix->extras[0]];
    CHECK(x->role == MF_PLAYALL && x->name.kind == MEDIA_OTHER && x->series == s->id);
    CHECK_STR(x->name.title, "70's Show – season 1, disc 1: all episodes");
}

TEST(media_library_indexes_a_real_world_tree) {
    th_boot();
    char root[512];
    make_library(root, sizeof root);
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nroots, 1);
    CHECK_INT(ix->nmovies, 2);
    CHECK_INT(ix->nseries, 1);
    CHECK_INT(ix->nepisodes, 3);
    CHECK_INT(ix->nothers, 1);
    CHECK_INT(ix->nrecent, 6);
    CHECK(ix->scanned_at > 0);
    /* hidden things never show up */
    CHECK(by_rel("Movies/Inception (2010)/Inception-trailer.mp4") == NULL);
    CHECK(by_rel("Series/Garden Show/Extras/Behind the scenes.mp4") == NULL);
    CHECK(by_rel("$RECYCLE.BIN/deleted.mp4") == NULL);
    CHECK(by_rel(".hidden/secret.mp4") == NULL);
    CHECK(by_rel("notes.txt") == NULL);
    /* movies, A-Z */
    const MediaFile *m0 = &ix->files[ix->movies[0]], *m1 = &ix->files[ix->movies[1]];
    CHECK_STR(m0->name.title, "Flower");
    CHECK_INT(m0->name.year, 2020);
    CHECK_STR(m1->name.title, "Inception");
    CHECK_INT(m1->name.year, 2010);
    /* poster: the folder's poster.jpg belongs to its only movie */
    const MediaFile *poster = by_rel("Movies/Inception (2010)/poster.jpg");
    CHECK(poster && poster->type == MF_IMAGE);
    CHECK(m1->poster == poster->id);
    /* subtitles found their video and language */
    const MediaFile *sv = by_rel("Movies/Flower (2020)/Flower.sv.srt"), *en = by_rel("Movies/Flower (2020)/Flower.en.srt");
    CHECK(sv && en);
    CHECK(sv->video == m0->id && en->video == m0->id);
    CHECK_STR(sv->lang, "sv");
    CHECK_STR(en->lang, "en");
    /* the series and its poster in the show folder above "Season N" */
    const MediaSeries *s = &ix->series[0];
    CHECK_STR(s->title, "Garden Show");
    CHECK_INT(s->count, 3);
    CHECK_INT(s->nseasons, 2);
    CHECK(s->poster == by_rel("Series/Garden Show/poster.png")->id);
    /* home video */
    const MediaFile *home = &ix->files[ix->others[0]];
    CHECK_STR(home->rel, "Home Videos/Midsommar 2024/IMG_1234.mov");
    th_halt();
}

TEST(media_library_episode_order_and_neighbours) {
    th_boot();
    char root[512];
    make_library(root, sizeof root);
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaSeries *s = &media_lib()->series[0];
    const MediaFile *e1 = media_episode(s, 0), *e2 = media_episode(s, 1), *e3 = media_episode(s, 2);
    CHECK(e1 && e2 && e3 && !media_episode(s, 3) && !media_episode(s, -1));
    CHECK(e1->name.season == 1 && e1->name.episode == 1);
    CHECK_STR(e1->name.ep_title, "Seeds");
    CHECK(e2->name.episode == 2 && e2->name.episode_end == 3); /* double episode */
    CHECK(e3->name.season == 2 && e3->name.episode == 1);
    CHECK(media_neighbour(e1, +1) == e2);
    CHECK(media_neighbour(e2, -1) == e1);
    CHECK(media_neighbour(e3, +1) == NULL);
    CHECK(media_neighbour(e1, -1) == NULL);
    CHECK(media_series(e2->series) == s);
    const MediaFile *movie = &media_lib()->files[media_lib()->movies[0]];
    CHECK(media_neighbour(movie, +1) == NULL);
    th_halt();
}

TEST(media_library_sorts_out_ripped_dvds) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    make_rips(root, "Series & movies/70's show");
    setting_set("media.roots", root);
    media_scan_sync();
    check_rips();
    /* the series poster sits in the show folder, above "season N/Disc - N" */
    CHECK(media_lib()->series[0].poster == by_rel("Series & movies/70's show/poster.jpg")->id);
    /* the series page: numbered episodes with their disc, and the season's extras */
    int adam = th_person("Adam");
    char url[64], hex[17];
    media_id_hex(media_lib()->series[0].id, hex);
    snprintf(url, sizeof url, "/media/series/%s?s=1", hex);
    const char *h = th_page(adam, url);
    CHECK_HAS(h, "Episode 13");
    CHECK_HAS(h, "disc 4");
    CHECK_HAS(h, "Extras");
    CHECK_HAS(h, "All episodes in one");
    CHECK_HAS(h, "Extra · title 7");
    th_halt();
}

TEST(media_library_root_can_be_the_show_folder) {
    th_boot();
    char root[512];
    snprintf(root, sizeof root, "%s/70's show", test_tmpdir());
    make_rips(root, "");
    setting_set("media.roots", root);
    media_scan_sync();
    check_rips(); /* the root's own name names the show */
    th_halt();
}

TEST(media_ripped_dvds_come_back_the_same_from_the_cache) {
    char data[512], root[512];
    str_copy(data, sizeof data, test_tmpdir());
    app_set_testing(true);
    CHECK(app_boot(data));
    str_copy(root, sizeof root, test_tmpdir());
    make_rips(root, "70's show");
    setting_set("media.roots", root);
    media_scan_sync();
    check_rips();
    app_halt();
    CHECK(app_boot(data)); /* before any scan: names parsed again from the cached paths */
    CHECK_INT(media_lib()->scanned_at, 0);
    check_rips();
    app_halt();
}

/* What an upload leaves on the server: the converted copies, and of each play-all only its first
   MB ("B1_t00.playall.mkv") - enough to know how long the disc is. Real durations again. */
TEST(media_library_play_all_header_sorts_out_uploaded_copies) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    static const Title s3d1[] = {{"C3_t01.web.mp4", 22.1}, {"D3_t02.web.mp4", 22.2}, {"E3_t03.web.mp4", 22.2},
                                 {"I2_t04.web.mp4", 22.1}, {"G3_t05.web.mp4", 22.2}, {"I4_t06.web.mp4", 22.3},
                                 {"I1_t07.web.mp4", 23.3}};
    static const Title s1d4[] = {{"B1_t01.web.mp4", 22.6}, {"B2_t02.web.mp4", 22.6}, {"B3_t03.web.mp4", 22.6},
                                 {"B4_t04.web.mp4", 22.6}, {"B5_t05.web.mp4", 22.6}, {"B6_t06.web.mp4", 22.6},
                                 {"C1_t07.web.mp4", 18.1}, {"C2_t08.web.mp4", 3.8},  {"C3_t09.web.mp4", 2.4}};
    put_disc(root, "70's show/season 3 70's show/Disc - 1", s3d1, countof(s3d1));
    put_disc(root, "70's show/season 1 70's show/Disc - 4", s1d4, countof(s1d4));
    setting_set("media.roots", root);
    media_scan_sync();
    /* no play-alls: the 23- and 18-minute featurettes pass for episodes, and the rest of
       season 3 would be numbered one off after them */
    CHECK_INT(media_lib()->series[0].count, 7 + 7);
    CHECK_STR(ep_file(3, 7), "I1_t07.web.mp4");
    CHECK_STR(ep_file(1, 7), "C1_t07.web.mp4");

    put_mkv_ex(root, "70's show/season 3 70's show/Disc - 1/B1_t00.playall.mkv", 133.1, true);
    put_mkv_ex(root, "70's show/season 1 70's show/Disc - 4/B7_t00.playall.mkv", 135.5, true);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->series[0].count, 6 + 6);
    CHECK_STR(ep_file(3, 6), "I4_t06.web.mp4");
    CHECK_STR(ep_file(3, 7), "");
    CHECK_STR(ep_file(1, 6), "B6_t06.web.mp4");
    CHECK_STR(ep_file(1, 7), "");
    CHECK_INT(extras_of(3, MF_BONUS), 1); /* the 23.3-minute t07 */
    CHECK_INT(extras_of(1, MF_BONUS), 3);
    CHECK_INT(extras_of(1, MF_PLAYALL) + extras_of(3, MF_PLAYALL), 0); /* a header doesn't play: never listed */
    CHECK_INT(ix->nrecent, 12);
    CHECK_INT(ix->nmovies + ix->nothers, 0);
    const MediaFile *h = by_rel("70's show/season 3 70's show/Disc - 1/B1_t00.playall.mkv");
    CHECK(h && h->role == MF_PLAYALL && h->probe.duration_ms == 7986000);
    CHECK(h->name.disc == 1 && h->name.track == 0); /* named like the title it stands in for */
    int adam = th_person("Adam");
    char url[64], hex[17];
    media_id_hex(ix->series[0].id, hex);
    snprintf(url, sizeof url, "/media/series/%s?s=3", hex);
    const char *page = th_page(adam, url);
    CHECK_HAS(page, "Extra · title 7");
    CHECK(!strstr(page, "All episodes in one"));
    th_halt();
}

TEST(media_scan_takes_a_finished_scan_before_starting_the_next) {
    th_boot();
    char root[512];
    make_library(root, sizeof root);
    setting_set("media.roots", root);
    media_scan_publish_sync(); /* a background scan finished; the tick hasn't swapped it in yet */
    CHECK_INT(media_lib()->nfiles, 0);
    puts_file(root, "Movies/Up (2009)/Up.2009.mkv", "up");
    media_scan_sync(); /* the next scan starts now: it takes the finished one first ... */
    CHECK_INT(media_lib()->probed, 1); /* ... and compares against it: only the new file is read */
    int n = media_lib()->nfiles;
    media_lib_tick(now_ms()); /* nothing stale is left to swap in over the newer index */
    CHECK_INT(media_lib()->nfiles, n);
    CHECK(by_rel("Movies/Up (2009)/Up.2009.mkv") != NULL);
    th_halt();
}

TEST(media_rescan_asked_for_during_a_scan_is_not_lost) {
    th_boot();
    media_scan_pretend(true); /* a background scan runs ... */
    media_scan_start();       /* ... when the library folders change */
    CHECK(media_rescan_queued());
    media_scan_pretend(false); /* it ends, its index already swapped in */
    media_lib_tick(now_ms());
    CHECK(!media_rescan_queued()); /* the tick starts the asked-for scan */
    th_halt();
}

/* Read-only look at a real folder: HEMMA_TEST_SHOW="/path/to/a show" ./test media_real
   prints what the library makes of it (skipped when unset). */
TEST(media_real_show_folder_listing) {
    const char *dir = plat_getenv("HEMMA_TEST_SHOW");
    if (!dir || !*dir) return;
    th_boot();
    setting_set("media.roots", dir);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    printf("\n%d files in %d ms: %d series, %d episodes, %d extras, %d movies, %d other\n", ix->nfiles, ix->scan_ms,
           ix->nseries, ix->nepisodes, ix->nextras, ix->nmovies, ix->nothers);
    for (int k = 0; k < ix->nseries; k++) {
        const MediaSeries *s = &ix->series[k];
        printf("series \"%s\": %d seasons, %d episodes\n", s->title, s->nseasons, s->count);
        for (int i = 0; i < s->count;) {
            const MediaFile *e = media_episode(s, i);
            int season = e->name.season, n = 0, pa = 0, bo = 0;
            while (i + n < s->count && media_episode(s, i + n)->name.season == season) n++;
            for (int x = 0; x < ix->nextras; x++) {
                const MediaFile *f = &ix->files[ix->extras[x]];
                if (f->series == s->id && f->name.season == season) pa += f->role == MF_PLAYALL, bo += f->role == MF_BONUS;
            }
            printf("  season %d: %d episodes, %d play-all, %d extras\n", season, n, pa, bo);
            for (int j = 0; j < n; j++)
                if (j < 3 || j == n - 1)
                    printf("    E%02d  %-58s %5.1f min\n", media_episode(s, i + j)->name.episode, media_episode(s, i + j)->rel,
                           media_episode(s, i + j)->probe.duration_ms / 60000.0);
            i += n;
        }
    }
    for (int x = 0; x < ix->nextras; x++) {
        const MediaFile *f = &ix->files[ix->extras[x]];
        if (f->role == MF_BONUS)
            printf("  extra: %-58s %5.1f min  \"%s\"\n", f->rel, f->probe.duration_ms / 60000.0, f->name.title);
    }
    th_halt();
}

TEST(media_ids_are_stable_and_parse_strictly) {
    th_boot();
    char root[512];
    make_library(root, sizeof root);
    setting_set("media.roots", root);
    media_scan_sync();
    const char *rel = "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4";
    const MediaFile *f = by_rel(rel);
    CHECK(f != NULL);
    char normalised[512];
    str_copy(normalised, sizeof normalised, root);
    for (char *p = normalised; *p; p++)
        if (*p == '\\') *p = '/';
    uint64_t id = f->id;
    CHECK(id == media_make_id(normalised, rel));
    CHECK(id != media_make_id("/other/root", rel));
    media_scan_sync(); /* again: same id */
    CHECK(by_rel(rel)->id == id);
    CHECK(media_file(id) == by_rel(rel));
    char hex[17];
    media_id_hex(id, hex);
    uint64_t back = 0;
    CHECK(media_parse_id(hex, &back) && back == id);
    char withext[30];
    snprintf(withext, sizeof withext, "%s.vtt", hex);
    CHECK(media_parse_id(withext, &back) && back == id);
    CHECK(!media_parse_id("123", &back));
    CHECK(!media_parse_id("zzzzzzzzzzzzzzzz", &back));
    CHECK(!media_parse_id("0123456789abcdef0", &back));
    CHECK(!media_parse_id("0000000000000000", &back));
    CHECK(!media_parse_id(NULL, &back));
    CHECK(media_file(0) == NULL);
    th_halt();
}

TEST(media_library_cache_survives_restart_and_reprobes_only_changes) {
    char data[512], root[512];
    str_copy(data, sizeof data, test_tmpdir());
    app_set_testing(true);
    CHECK(app_boot(data));
    make_library(root, sizeof root);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK(media_lib()->probed == 6); /* every video was read once */
    const MediaFile *flower = by_rel("Movies/Flower (2020)/Flower.mp4");
    int64_t dur = flower->probe.duration_ms;
    app_halt();

    CHECK(app_boot(data)); /* the library is back instantly, from SQLite */
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->scanned_at, 0);
    CHECK_INT(ix->nmovies, 2);
    CHECK_INT(ix->nepisodes, 3);
    CHECK_INT(by_rel("Movies/Flower (2020)/Flower.mp4")->probe.duration_ms, dur);
    CHECK(by_rel("Movies/Flower (2020)/Flower.sv.srt")->video == by_rel("Movies/Flower (2020)/Flower.mp4")->id);
    /* change one file: only that one is probed again */
    puts_file(root, "Series/Garden Show/Season 2/Garden.Show.S02E01.mkv", "a longer, changed episode");
    media_scan_sync();
    CHECK_INT(media_lib()->probed, 1);
    /* delete one: it leaves the index and the cache */
    char gone[1024];
    snprintf(gone, sizeof gone, "%s/Home Videos/Midsommar 2024/IMG_1234.mov", root);
    CHECK(file_remove(gone));
    media_scan_sync();
    CHECK_INT(media_lib()->nothers, 0);
    sqlite3_stmt *st = db_prep("SELECT count(*) FROM media_file WHERE rel LIKE '%IMG_1234%'");
    CHECK(st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    /* removing the root empties the library */
    setting_set("media.roots", "");
    media_scan_sync();
    CHECK_INT(media_lib()->nfiles, 0);
    st = db_prep("SELECT count(*) FROM media_file");
    CHECK(st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    app_halt();
}

TEST(media_real_clip_is_probed) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    if (!fixture("flower.mp4", root, "Flower (2020).mp4")) { /* fixtures are optional (build/fixtures) */
        th_halt();
        return;
    }
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaFile *f = by_rel("Flower (2020).mp4");
    CHECK(f != NULL);
    CHECK_STR(f->probe.container, "mp4");
    CHECK(f->probe.duration_ms > 5000 && f->probe.duration_ms < 5200);
    CHECK_STR(f->probe.vcodec, "h264");
    CHECK(f->probe.playable == PLAY_YES);
    th_halt();
}

TEST(media_progress_debounces_writes_and_finishes_at_92_percent) {
    th_boot();
    int adam = th_person("Adam");
    uint64_t id = 0x1234;
    g_fake_now = 1000000;
    CHECK(!media_watch_report(adam, id, 30, 100, false, g_fake_now));
    MediaWatch *w = media_watch_get(adam, id);
    CHECK(w && w->pos == 30 && w->dur == 100 && !w->done);
    g_fake_now += 5; /* quick follow-up: memory only */
    media_watch_report(adam, id, 40, 100, false, g_fake_now);
    CHECK_INT(media_watch_get(adam, id)->pos, 40);
    sqlite3_stmt *st = db_prep("SELECT pos FROM media_watch WHERE person = ?");
    sqlite3_bind_int(st, 1, adam);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 30);
    g_fake_now += 15; /* now it's written */
    media_watch_report(adam, id, 50, 0, false, g_fake_now);
    st = db_prep("SELECT pos, dur FROM media_watch WHERE person = ?");
    sqlite3_bind_int(st, 1, adam);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 50 && sqlite3_column_int(st, 1) == 100);
    /* 92% counts as watched, and that is written at once */
    g_fake_now += 1;
    CHECK(media_watch_report(adam, id, 92, 100, false, g_fake_now));
    CHECK(media_watch_get(adam, id)->done);
    st = db_prep("SELECT done FROM media_watch WHERE person = ?");
    sqlite3_bind_int(st, 1, adam);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    /* "ended" finishes a video even without a duration */
    CHECK(media_watch_report(adam, 0x99, 5, 0, true, g_fake_now));
    /* unwatching starts over */
    media_watch_set_done(adam, id, false, g_fake_now);
    CHECK(!media_watch_get(adam, id)->done);
    CHECK_INT(media_watch_get(adam, id)->pos, 0);
    /* nonsense positions are clamped */
    media_watch_report(adam, 0x77, -50, -1, false, g_fake_now);
    CHECK_INT(media_watch_get(adam, 0x77)->pos, 0);
    th_halt();
}
