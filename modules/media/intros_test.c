/* A series' intros, found by themselves (intros.c): a made-up show whose episodes share a theme - at another place
 * in each - is listened to a season at a time, each episode gets its intro (one without the theme none; a season of
 * one borrows its neighbours'); the watch page's player offers "Skip intro" there; Manage shows it and an admin fixes
 * it (by hand, "no intro", back to Hemma's), which the job then leaves alone; a changed episode does its season again
 * reading only that one; it waits while someone watches and when switched off; the Library settings card in three
 * languages, admins only. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/intro_testmk.h"
#include "download.h"
#include "intros.h"
#include "library.h"
#include "media.h"

static int admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

static const MediaFile *ep(int season, int episode) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        if (f->type == MF_VIDEO && f->name.kind == MEDIA_EPISODE && f->name.season == season && f->name.episode == episode) return f;
    }
    return NULL;
}

static bool episode_file(const char *root, int season, int episode, double at, uint32_t theme) {
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/Show/Season %d", root, season);
    snprintf(path, sizeof path, "%s/Show S%02dE%02d.mp4", dir, season, episode);
    return dir_make(dir) && intro_test_episode(path, 40, at, 18, 100u + (uint32_t)(season * 10 + episode), theme);
}

static bool near_ms(int64_t got, double want_s) { return llabs(got - (int64_t)(want_s * 1000)) < 600; }

static int run_all(void) {
    int n = 0;
    for (int i = 0; i < 20 && intros_test_step() == 1; i++) n++;
    return n;
}

static bool intro_is(int season, int episode, double from, double to) {
    int64_t a, z;
    const MediaFile *f = ep(season, episode);
    bool ok = f && media_intro_of(f, &a, &z);
    if (ok && (!near_ms(a, from) || !near_ms(z, to)))
        printf("  S%02dE%02d: %.2f-%.2f, want %.2f-%.2f\n", season, episode, a / 1000.0, z / 1000.0, from, to);
    return ok && near_ms(a, from) && near_ms(z, to);
}

static bool no_intro(int season, int episode) {
    int64_t a, z;
    const MediaFile *f = ep(season, episode);
    return f && !media_intro_of(f, &a, &z);
}

static const char *watch(int person, const MediaFile *f) {
    char url[64];
    snprintf(url, sizeof url, "/media/watch/%016llx", (unsigned long long)f->id);
    return th_page(person, url);
}

TEST(intros_found_skipped_and_fixed) {
    th_boot();
    int anna = admin("Anna"), eva = th_person("Eva");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    /* season 1: the theme at 4, 11.5 and 20 s, the fourth without it; season 2: one episode, the theme at 7 s */
    CHECK(episode_file(root, 1, 1, 4, 77) && episode_file(root, 1, 2, 11.5, 77) && episode_file(root, 1, 3, 20, 77));
    CHECK(episode_file(root, 1, 4, 0, 0) && episode_file(root, 2, 1, 7, 77));
    setting_set("media.roots", root);
    media_scan_sync();
    intros_test_read_s(40);
    CHECK_INT(run_all(), 2); /* a season at a time */
    CHECK_INT(intros_test_reads(), 5); /* (season 2 borrowed season 1's last three: from memory) */
    CHECK(intro_is(1, 1, 4, 22) && intro_is(1, 2, 11.5, 29.5) && intro_is(1, 3, 20, 38));
    CHECK(no_intro(1, 4));
    CHECK(intro_is(2, 1, 7, 25)); /* (a season of one: with the season before's episodes) */
    CHECK_INT(intros_test_step(), 0);

    /* the player: "Skip intro" there - not where there's none */
    const char *page = watch(eva, ep(1, 2));
    CHECK_HAS(page, "data-p=\"skip\"");
    const char *from = strstr(page, "data-from=\""), *to = strstr(page, "data-to=\"");
    CHECK(from && to);
    CHECK(fabs(atof(from + 11) - 11.5) < 0.6 && fabs(atof(to + 9) - 29.5) < 0.6);
    CHECK_HAS(page, ">Skip intro</button>");
    CHECK(!strstr(watch(eva, ep(1, 4)), "data-p=\"skip\""));

    /* Manage: each episode says so, and its dialog */
    char url[160];
    const MediaFile *e1 = ep(1, 1);
    snprintf(url, sizeof url, "/media/series/%016llx?manage=1&s=1", (unsigned long long)e1->series);
    page = th_page(anna, url);
    CHECK_HAS(page, "Intro 0:04–0:22");
    CHECK_HAS(page, "No intro");
    snprintf(url, sizeof url, "/media/series/%016llx?manage=1&s=1&intro=%016llx", (unsigned long long)e1->series,
             (unsigned long long)e1->id);
    page = th_page(anna, url);
    CHECK_HAS(page, "data-a=\"media.intro\"");
    CHECK_HAS(page, "Hemma found this intro by itself");
    CHECK_HAS(page, "value=\"0:04\"");

    /* by hand: kept, whatever the job does */
    char form[200];
    snprintf(form, sizeof form, "id=%016llx&how=set&from=0:02&to=0:50", (unsigned long long)e1->id);
    ThResult r = th_act(anna, "media.intro", form);
    CHECK(!r.ok); /* (past its end) */
    snprintf(form, sizeof form, "id=%016llx&how=set&from=x&to=0:30", (unsigned long long)e1->id);
    CHECK(!th_act(anna, "media.intro", form).ok);
    snprintf(form, sizeof form, "id=%016llx&how=set&from=0:02&to=0:30.5", (unsigned long long)e1->id);
    r = th_act(anna, "media.intro", form);
    CHECK(r.ok);
    CHECK_STR(r.toast, "Skip intro goes from 0:02 to 0:31");
    CHECK(intro_is(1, 1, 2, 30.5));
    CHECK_INT(intros_test_step(), 0);
    CHECK(intro_is(1, 1, 2, 30.5));
    CHECK(!th_act(eva, "media.intro", form).ok); /* (members: no) */
    snprintf(form, sizeof form, "id=%016llx&how=none", (unsigned long long)e1->id);
    CHECK(th_act(anna, "media.intro", form).ok);
    CHECK(no_intro(1, 1));
    CHECK(!strstr(watch(eva, ep(1, 1)), "data-p=\"skip\""));
    snprintf(url, sizeof url, "/media/series/%016llx?manage=1&s=1&intro=%016llx", (unsigned long long)e1->series,
             (unsigned long long)e1->id);
    CHECK_HAS(th_page(anna, url), "You said this episode has no intro");
    snprintf(form, sizeof form, "id=%016llx&how=auto", (unsigned long long)e1->id);
    CHECK(th_act(anna, "media.intro", form).ok);
    int reads = intros_test_reads();
    CHECK_INT(run_all(), 1);
    CHECK_INT(intros_test_reads(), reads); /* (every fingerprint of the season was in memory) */
    CHECK(intro_is(1, 1, 4, 22));

    /* an episode changed: its season again, reading only that one */
    CHECK(episode_file(root, 1, 2, 9, 77));
    media_scan_sync();
    reads = intros_test_reads();
    CHECK_INT(run_all(), 1);
    CHECK_INT(intros_test_reads(), reads + 1);
    CHECK(intro_is(1, 2, 9, 27));
    th_halt();
}

TEST(intros_wait_their_turn_and_the_card) {
    th_boot();
    int anna = admin("Anna");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    CHECK(episode_file(root, 1, 1, 3, 55) && episode_file(root, 1, 2, 9, 55) && episode_file(root, 1, 3, 15, 55));
    setting_set("media.roots", root);
    media_scan_sync();
    intros_test_read_s(40);
    const char *const allow[] = {"Anna", "A", "Show", root, "MKV", "TVmaze", "OMDb", "Wikipedia", "omdbapi.com", NULL};
    Buf rep = {0};
    int bad = 0;
    intros_tick(now_ms());
    bad += th_untranslated(anna, "/media/settings/converting", allow, &rep); /* to do */
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Listening for intros: 0 of 3 episodes done");
    media_download_test_pace(0, 1); /* someone watching */
    CHECK_INT(intros_test_step(), -1);
    intros_tick(now_ms());
    bad += th_untranslated(anna, "/media/settings/converting", allow, &rep);
    media_download_test_pace(0, 0);
    CHECK(th_act(anna, "media.intros", "").ok); /* switched off */
    CHECK_INT(intros_test_step(), -1);
    bad += th_untranslated(anna, "/media/settings/converting", allow, &rep);
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Switched off");
    CHECK(th_act(anna, "media.intros", "on=1").ok);
    CHECK_INT(run_all(), 1);
    bad += th_untranslated(anna, "/media/settings/converting", allow, &rep); /* done */
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Intros found in 3 episodes");
    if (bad) printf("%s", buf_cstr(&rep));
    CHECK_INT(bad, 0);
    pref_set(anna, PREF_LANG, "sv");
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Intron hittade i 3 avsnitt");
    const MediaFile *e = ep(1, 2);
    CHECK_HAS(watch(anna, e), ">Hoppa över intro</button>");
    pref_set(anna, PREF_LANG, "es");
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Intros encontradas en 3 episodios");
    CHECK_HAS(watch(anna, e), ">Saltar intro</button>");
    int eva = th_person("Eva"); /* members: no card, no switch */
    CHECK(!strstr(th_page(eva, "/media/settings/converting"), "media-intros"));
    CHECK(!th_act(eva, "media.intros", "on=1").ok);
    buf_free(&rep);
    th_halt();
}

/* An episode gone (an unplugged disk, deleted) before the library noticed: its season waits for the next scan - not
   read again and again - and the scan then does it without it. */
TEST(intros_a_file_gone_waits_for_the_scan) {
    th_boot();
    char root[512], gone[1200];
    str_copy(root, sizeof root, test_tmpdir());
    CHECK(episode_file(root, 1, 1, 3, 66) && episode_file(root, 1, 2, 9, 66) && episode_file(root, 1, 3, 15, 66));
    setting_set("media.roots", root);
    media_scan_sync();
    intros_test_read_s(40);
    snprintf(gone, sizeof gone, "%s/Show/Season 1/Show S01E02.mp4", root);
    CHECK(remove(gone) == 0);
    CHECK_INT(intros_test_step(), 1); /* (tried: E02 couldn't be opened) */
    CHECK(no_intro(1, 1) && no_intro(1, 3));
    CHECK_INT(intros_test_step(), 0); /* (not again before the next scan) */
    media_scan_sync();
    CHECK_INT(run_all(), 1);
    CHECK(intro_is(1, 1, 3, 21) && intro_is(1, 3, 15, 33));
    th_halt();
}
