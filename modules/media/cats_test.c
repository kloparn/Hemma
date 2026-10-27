/* 1.4: categories of series and films (cats.c) - ticked by hand, from .nfo files, from the online lookup
   (lookup.c) - and what uses them: the lists' chips, order and "Not started / Started", Watch's rows and a
   category's page, the TV's row, and search (media.c). */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "media.h"

static void put(const char *root, const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, text, strlen(text));
}

static char g_root[512];

/* Two series (one with a tvshow.nfo), two films (one with its own .nfo); returns Adam's id (a member). */
static int library(void) {
    th_boot();
    int adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", "e1");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E02.Roots.mp4", "e2");
    put(g_root, "Series/Night Owls/Season 1/Night.Owls.S01E01.Moon.mp4", "n1");
    put(g_root, "Series/Night Owls/tvshow.nfo",
        "<?xml version=\"1.0\"?><tvshow><title>Night Owls</title><genre>Animation</genre>"
        "<genre>Comedy</genre><genre>Talk &amp; Tea</genre></tvshow>");
    put(g_root, "Series/Night Owls/Season 1/Night.Owls.S01E01.Moon.nfo", "<episodedetails><genre>Horror</genre></episodedetails>");
    put(g_root, "Movies/Inception (2010)/Inception (2010).mkv", "m1");
    put(g_root, "Movies/Inception (2010)/Inception (2010).nfo", "<movie><genre>Science Fiction / Thriller</genre></movie>");
    put(g_root, "Movies/Flower (2020)/Flower.mp4", "m2");
    setting_set("media.roots", g_root);
    media_scan_sync();
    return adam;
}

static const MediaSeries *series_named(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) return &ix->series[i];
    return NULL;
}

static const MediaFile *film(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nmovies; i++)
        if (str_eq(ix->files[ix->movies[i]].name.title, title)) return &ix->files[ix->movies[i]];
    return NULL;
}

static const char *cats_of(const MediaWork *w) {
    static Buf s;
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int n = media_cats_of(w, have, MEDIA_CAT_MAX);
    buf_reset(&s);
    for (int i = 0; i < n; i++) buf_printf(&s, "%s%s", i ? "," : "", have[i]);
    return buf_cstr(&s);
}

static ThResult tick(int person, const char *key, const char *cat, int on) {
    char form[200];
    snprintf(form, sizeof form, "work=%s&cat=%s&on=%d", key, cat, on);
    return th_act(person, "media.cat", form);
}

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

TEST(cats_come_from_nfo_files_and_people_tick_them) {
    int adam = library();
    MediaWork owls, garden, inc;
    media_work_series(series_named("Night Owls"), &owls);
    media_work_series(series_named("Garden Show"), &garden);
    media_work_film(film("Inception"), &inc);
    /* a tvshow.nfo's genres, as starters ("Animation" -> animated) or the household's own; an episode's .nfo isn't read */
    CHECK_STR(cats_of(&owls), "animated,comedy,+Talk & Tea");
    CHECK_STR(media_nfo_genres(series_named("Night Owls")->nfo), "Animation|Comedy|Talk & Tea");
    CHECK_STR(cats_of(&inc), "scifi,thriller"); /* (an old-style "A / B" in one) */
    CHECK_STR(cats_of(&garden), "");
    /* ticked by hand, and off again; one from the .nfo taken off */
    CHECK(tick(adam, garden.key, "family", 1).ok);
    CHECK_STR(cats_of(&garden), "family");
    CHECK(tick(adam, owls.key, "comedy", 0).ok);
    CHECK_STR(cats_of(&owls), "animated,+Talk & Tea");
    CHECK(tick(adam, owls.key, "comedy", 1).ok);
    CHECK_STR(cats_of(&owls), "animated,comedy,+Talk & Tea");
    CHECK(!tick(adam, garden.key, "nonsense", 1).ok); /* (only a starter or a "+name") */
    CHECK(!tick(adam, "s0000000000000001", "family", 1).ok);
    /* typed in: a starter's name in any language is that starter; a new one keeps its spelling, the next joins it */
    char form[200];
    snprintf(form, sizeof form, "work=%s&name=Komedi", garden.key);
    ThResult r = th_act(adam, "media.cat_new", form);
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Added to Comedy");
    snprintf(form, sizeof form, "work=%s&name=Pok%%C3%%A9mon", garden.key);
    CHECK(th_act(adam, "media.cat_new", form).ok);
    snprintf(form, sizeof form, "work=%s&name=POK%%C3%%89MON", inc.key);
    CHECK(th_act(adam, "media.cat_new", form).ok);
    CHECK_STR(cats_of(&garden), "comedy,family,+Pokémon");
    CHECK_STR(cats_of(&inc), "scifi,thriller,+Pokémon");
    snprintf(form, sizeof form, "work=%s&name=", garden.key);
    r = th_act(adam, "media.cat_new", form);
    CHECK(!r.ok);
    CHECK_STR(r.field, "name");
    th_halt();
}

TEST(cats_show_on_titles_and_only_grown_ups_tick_them) {
    int adam = library();
    int kid = with_role("Leo", ROLE_CHILD), tv = with_role("TV", ROLE_DISPLAY);
    MediaWork garden;
    media_work_series(series_named("Garden Show"), &garden);
    CHECK(tick(adam, garden.key, "family", 1).ok);
    char page[64];
    snprintf(page, sizeof page, "/media/series/%016llx", (unsigned long long)series_named("Garden Show")->id);
    const char *h = th_page(adam, page);
    CHECK_HAS(h, "href=\"/media/series?cat=family\">Family</a>");
    CHECK_HAS(h, "?cats=1\"");
    char dlg[80];
    snprintf(dlg, sizeof dlg, "%s?cats=1", page);
    h = th_page(adam, dlg);
    CHECK_HAS(h, "role=\"dialog\"");
    CHECK_HAS(h, "aria-pressed=\"true\">Family</button>");
    CHECK_HAS(h, "aria-pressed=\"false\">Comedy</button>");
    CHECK(!strstr(h, "From the internet")); /* (that part is for admins) */
    /* a child sees the names, never the dialog; its action is refused */
    h = th_page(kid, page);
    CHECK_HAS(h, ">Family</a>");
    CHECK(!strstr(h, "?cats=1"));
    CHECK(!strstr(th_page(kid, dlg), "role=\"dialog\""));
    CHECK(!tick(kid, garden.key, "comedy", 1).ok);
    CHECK(!tick(tv, garden.key, "comedy", 1).ok);
    /* a film's page too */
    snprintf(page, sizeof page, "/media/watch/%016llx", (unsigned long long)film("Inception")->id);
    h = th_page(adam, page);
    CHECK_HAS(h, "href=\"/media/movies?cat=scifi\">Science fiction</a>");
    th_halt();
}

TEST(cats_filter_the_lists_and_make_rows_on_watch) {
    int adam = library();
    MediaWork garden, flower;
    media_work_series(series_named("Garden Show"), &garden);
    media_work_film(film("Flower"), &flower);
    CHECK(tick(adam, garden.key, "comedy", 1).ok);
    CHECK(tick(adam, flower.key, "comedy", 1).ok);
    /* the chips: the categories this list's titles are in, with how many */
    const char *h = th_page(adam, "/media/series");
    CHECK_HAS(h, "class=\"chips media-cats\"");
    CHECK_HAS(h, ">Comedy <small>2</small></a>");
    CHECK_HAS(h, ">Animated <small>1</small></a>");
    h = th_page(adam, "/media/series?cat=animated");
    CHECK_HAS(h, "Night Owls");
    CHECK(!strstr(h, "Garden Show</span>") && !strstr(h, ">Garden Show<small>"));
    h = th_page(adam, "/media/movies?cat=comedy");
    CHECK_HAS(h, "Flower");
    CHECK(!strstr(h, "Inception<small>"));
    CHECK_HAS(th_page(adam, "/media/movies?cat=horror"), "Nothing here with these choices");
    /* Watch's home: a row for a category with two titles or more, and its page */
    h = th_page(adam, "/media");
    CHECK_HAS(h, "<h2>Comedy</h2><a href=\"/media?cat=comedy\">");
    CHECK(!strstr(h, "<h2>Animated</h2>")); /* (one title is no row) */
    h = th_page(adam, "/media?cat=comedy");
    CHECK_HAS(h, "class=\"media-cat-h\">Comedy</h2>");
    CHECK_HAS(h, "Garden Show");
    CHECK_HAS(h, "Flower");
    CHECK_HAS(h, "Night Owls");
    CHECK(!strstr(h, "Inception<small>"));
    /* the TV's row of one category, under its name */
    const Block *bl = block_find("media.category", NULL);
    CHECK(bl);
    Ctx c;
    app_ctx(&c, adam, "/screens/1");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_TV, "comedy"));
    CHECK_HAS(buf_cstr(&b), "Garden Show");
    CHECK_HAS(buf_cstr(&b), "Flower");
    buf_reset(&b);
    CHECK(!bl->render(&c, &b, SCREEN_TV, "horror"));
    BlockChoice ch[8];
    int n = bl->choices(&c, ch, 8);
    CHECK(n >= 2);
    CHECK_STR(ch[0].arg, "comedy");
    CHECK_STR(ch[0].label, "Comedy");
    buf_free(&b);
    app_ctx_free(&c);
    th_halt();
}

TEST(cats_lists_sort_and_show_what_the_viewer_started) {
    int adam = library();
    const MediaSeries *owls = series_named("Night Owls");
    char form[200];
    snprintf(form, sizeof form, "id=%016llx&pos=120&dur=1200", (unsigned long long)media_episode(owls, 0)->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    const char *h = th_page(adam, "/media/series?show=going");
    CHECK_HAS(h, "Night Owls");
    CHECK(!strstr(h, ">Garden Show<small>"));
    h = th_page(adam, "/media/series?show=new");
    CHECK_HAS(h, "Garden Show");
    CHECK(!strstr(h, ">Night Owls<small>"));
    /* watched lately first; A-Z otherwise */
    h = th_page(adam, "/media/series?sort=watched");
    const char *a = strstr(h, ">Night Owls<small>"), *b = strstr(h, ">Garden Show<small>");
    CHECK(a && b && a < b);
    h = th_page(adam, "/media/series");
    a = strstr(h, ">Night Owls<small>"), b = strstr(h, ">Garden Show<small>");
    CHECK(a && b && b < a);
    /* the choices keep each other in their links */
    CHECK_HAS(th_page(adam, "/media/series?sort=watched&show=going"), "href=\"/media/series?cat=animated&amp;sort=watched&amp;show=going\"");
    th_halt();
}

TEST(cats_search_takes_every_word_in_any_order) {
    int adam = library();
    const char *h = th_page(adam, "/media?q=show%20garden"); /* (any order) */
    CHECK_HAS(h, "Garden Show");
    CHECK(!strstr(th_page(adam, "/media?q=garden%20owls"), "media-found")); /* (every word) */
    h = th_page(adam, "/media?q=garden%20roots"); /* an episode's name: under Episodes */
    CHECK_HAS(h, "<h2>Episodes <small>1</small></h2>");
    CHECK_HAS(h, "S01E02");
    h = th_page(adam, "/media?q=garden"); /* the series' name alone lists the series, not each episode */
    CHECK_HAS(h, "<h2>Series <small>1</small></h2>");
    CHECK(!strstr(h, "<h2>Episodes"));
    h = th_page(adam, "/media?q=garden%20s01e02"); /* an episode's number */
    CHECK_HAS(h, "<h2>Episodes <small>1</small></h2>");
    CHECK_HAS(h, "Roots");
    CHECK_HAS(th_page(adam, "/media?q=owls%201x01"), "Moon");
    h = th_page(adam, "/media?q=animated"); /* a category's name, in English ... */
    CHECK_HAS(h, "Night Owls");
    CHECK(!strstr(h, ">Garden Show<small>"));
    CHECK_HAS(th_page(adam, "/media?q=2010"), "Inception"); /* ... a year */
    CHECK_HAS(th_page(adam, "/media?q=zzzz"), "Nothing matches");
    th_halt();
}

TEST(cats_from_the_lookup_and_its_answers) {
    int adam = library();
    MediaWork garden, flower;
    media_work_series(series_named("Garden Show"), &garden);
    media_work_film(film("Flower"), &flower);
    media_lookup_test_put(garden.key, "Comedy|Children|Science-Fiction", false);
    CHECK_STR(cats_of(&garden), "comedy,kids,scifi");
    /* TVmaze: the show of its year, else of its very name, else the first; its type "Animation" counts */
    static const char *const list =
        "[{\"score\":0.9,\"show\":{\"id\":1,\"name\":\"Garden Shows\",\"type\":\"Scripted\",\"genres\":[\"Drama\"],"
        "\"premiered\":\"2001-01-01\"}},{\"score\":0.8,\"show\":{\"id\":2,\"name\":\"Garden Show\",\"type\":\"Animation\","
        "\"genres\":[\"Comedy\"],\"premiered\":\"1999-05-01\"}},{\"score\":0.7,\"show\":{\"id\":3,\"name\":\"Garden\","
        "\"type\":\"Reality\",\"genres\":[\"Nature\"],\"premiered\":\"2015-02-02\"}}]";
    char g[200];
    CHECK_INT(media_lookup_test_tvmaze(list, "Garden Show", 2001, g, sizeof g), 1); /* (its words in its year beat its name) */
    CHECK_STR(g, "Drama");
    CHECK_INT(media_lookup_test_tvmaze(list, "Garden Show", 0, g, sizeof g), 1);
    CHECK_STR(g, "Comedy|Animation");
    CHECK_INT(media_lookup_test_tvmaze(list, "Something", 0, g, sizeof g), 0); /* (TVmaze answers anything: no) */
    CHECK_INT(media_lookup_test_tvmaze(list, "Garden", 1999, g, sizeof g), 1); /* (its words, in its year) */
    CHECK_STR(g, "Comedy|Animation");
    CHECK_INT(media_lookup_test_tvmaze("[{\"show\":{\"name\":\"That '70s Show\",\"genres\":[\"Comedy\"]}},{\"show\":"
                                       "{\"name\":\"Pokémon\",\"genres\":[\"Anime\"]}}]", "70's Show", 0, g, sizeof g), 1);
    CHECK_STR(g, "Comedy");
    CHECK_INT(media_lookup_test_tvmaze("[{\"show\":{\"name\":\"Pokémon\",\"genres\":[\"Anime\"]}}]", "pokemon", 0, g, sizeof g), 1);
    CHECK_STR(g, "Anime");
    CHECK_INT(media_lookup_test_tvmaze("[]", "Garden Show", 0, g, sizeof g), 0);
    CHECK_INT(media_lookup_test_tvmaze("<html>", "Garden Show", 0, g, sizeof g), -1);
    CHECK_INT(media_lookup_test_tvmaze("{\"id\":84,\"name\":\"Family Guy\",\"type\":\"Animation\",\"genres\":[\"Comedy\","
                                       "\"Family\"],\"premiered\":\"1999-01-31\"}", "x", 0, g, sizeof g), 1);
    CHECK_STR(g, "Comedy|Family|Animation");
    /* OMDb */
    CHECK_INT(media_lookup_test_omdb("{\"Title\":\"Beetlejuice\",\"Year\":\"1988\",\"Genre\":\"Comedy, Fantasy\","
                                     "\"imdbID\":\"tt0094721\",\"Response\":\"True\"}", g, sizeof g), 1);
    CHECK_STR(g, "Comedy|Fantasy");
    CHECK_INT(media_lookup_test_omdb("{\"Response\":\"False\",\"Error\":\"Movie not found!\"}", g, sizeof g), 0);
    CHECK_INT(media_lookup_test_omdb("{\"Response\":\"False\",\"Error\":\"Invalid API key!\"}", g, sizeof g), -2);
    /* what's asked next: series by name (the one without an answer), films only with a key */
    char host[64], path[700];
    CHECK(media_lookup_test_next(host, sizeof host, path, sizeof path));
    CHECK_STR(host, "api.tvmaze.com");
    CHECK_STR(path, "/search/shows?q=Night%20Owls");
    MediaWork owls;
    media_work_series(series_named("Night Owls"), &owls);
    media_lookup_test_put(owls.key, "Animation", false);
    CHECK(!media_lookup_test_next(host, sizeof host, path, sizeof path)); /* (no key: no films) */
    setting_set("media.omdb_key", "abc123");
    CHECK(media_lookup_test_next(host, sizeof host, path, sizeof path));
    CHECK_STR(host, "www.omdbapi.com");
    CHECK_HAS(path, "&apikey=abc123");
    CHECK(str_starts(path, "/?type=movie&t=Flower&y=2020&") || str_starts(path, "/?type=movie&t=Inception&y=2010&"));
    (void)flower;
    (void)adam;
    th_halt();
}

TEST(cats_lookup_card_and_ids_for_admins) {
    int adam = library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva");
    /* offline: the box is greyed, and can't be switched on */
    media_lookup_test_online(-1);
    const char *h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "Categories from the internet");
    CHECK_HAS(h, "reach the internet right now");
    CHECK_HAS(h, "name=\"on\" value=\"1\" data-auto disabled");
    CHECK(!strstr(th_page(eva, "/media/settings"), "Categories from the internet"));
    CHECK(!th_act(adam, "media.lookup", "on=1").ok);
    /* online: on, with a key; a bad key is refused */
    media_lookup_test_online(1);
    CHECK(!th_act(adam, "media.lookup", "on=1&omdb=no%20way").ok);
    CHECK(th_act(adam, "media.lookup", "on=1&omdb=k3y").ok);
    CHECK_INT(setting_int("media.lookup", 0), 1);
    CHECK(!th_act(eva, "media.lookup", "on=0").ok);
    /* an id given in Manage: asked by that, before anything by name */
    MediaWork owls, inc;
    media_work_series(series_named("Night Owls"), &owls);
    media_work_film(film("Inception"), &inc);
    char form[300], host[64], path[700];
    snprintf(form, sizeof form, "work=%s&id=https%%3A%%2F%%2Fwww.tvmaze.com%%2Fshows%%2F84%%2Ffamily-guy", owls.key);
    CHECK(th_act(adam, "media.lookup_id", form).ok);
    CHECK(media_lookup_test_next(host, sizeof host, path, sizeof path));
    CHECK_STR(path, "/shows/84");
    snprintf(form, sizeof form, "work=%s&id=tt0182576", owls.key);
    CHECK(th_act(adam, "media.lookup_id", form).ok);
    CHECK(media_lookup_test_next(host, sizeof host, path, sizeof path));
    CHECK_STR(path, "/lookup/shows?imdb=tt0182576");
    snprintf(form, sizeof form, "work=%s&id=https%%3A%%2F%%2Fwww.imdb.com%%2Ftitle%%2Ftt1375666%%2F", inc.key);
    CHECK(th_act(adam, "media.lookup_id", form).ok);
    CHECK(media_lookup_test_next(host, sizeof host, path, sizeof path));
    CHECK_STR(host, "www.omdbapi.com");
    CHECK_STR(path, "/?i=tt1375666&apikey=k3y");
    snprintf(form, sizeof form, "work=%s&id=84", inc.key); /* (a film: only an IMDb id) */
    ThResult r = th_act(adam, "media.lookup_id", form);
    CHECK(!r.ok);
    CHECK_STR(r.field, "id");
    snprintf(form, sizeof form, "work=%s&id=tt0182576", owls.key);
    CHECK(!th_act(eva, "media.lookup_id", form).ok);
    /* Manage shows it */
    char page[96];
    snprintf(page, sizeof page, "/media/series/%016llx?manage=1", (unsigned long long)series_named("Night Owls")->id);
    h = th_page(adam, page);
    CHECK_HAS(h, "From the internet");
    CHECK_HAS(h, "Update categories");
    media_lookup_test_put(owls.key, "Comedy", true);
    CHECK_HAS(th_page(adam, page), "by the id you gave");
    th_halt();
}
