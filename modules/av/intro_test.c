/* Intros by their sound (intro.c): made-up episodes - sound of their own, the same theme somewhere in their
 * first minutes, louder or softer, at another rate, with noise on top - must give that theme's place to the
 * frame; episodes sharing nothing give nothing. HEMMA_TEST_INTRO=<a season's folder> runs it on real files
 * (never part of the suite). */
#include "../../core/plat.h"
#include "../../tests/test.h"
#include "intro.h"
#include "intro_testmk.h"

#include <math.h>
#include <stdio.h>

typedef struct {
    int rate;
    double secs, theme_at, theme_len, theme_gain, noise;
    uint32_t own, theme; /* seeds: the episode's own sound, the theme's (0 = none) */
} Ep;

/* An episode's print, its start_ms as a file would give it (frame 0's centre). */
static bool episode(const Ep *e, IntroPrint *out) {
    long n = (long)(e->secs * e->rate);
    float *x = intro_test_sound(n, e->rate, e->theme_at, e->theme_len, e->own, e->theme, e->theme_gain, e->noise);
    IntroPrinter *p = x ? intro_printer_open(e->rate) : NULL;
    bool ok = p && intro_printer_feed(p, x, (int)n) && intro_printer_finish(p, out);
    free(x);
    return ok;
}

TEST(intro_prints_frames_and_silence) {
    IntroPrinter *p = intro_printer_open(48000);
    CHECK(p);
    float *z = calloc(48000 * 5, sizeof(float));
    CHECK(intro_printer_feed(p, z, 48000 * 5));
    IntroPrint pr;
    CHECK(intro_printer_finish(p, &pr));
    CHECK(pr.n >= 5 * INTRO_FPS - 8 && pr.n <= 5 * INTRO_FPS);
    for (int i = 0; i < pr.n; i++) CHECK(!pr.loud[i]); /* silence matches nothing */
    CHECK(pr.start_ms > 150 && pr.start_ms < 220);    /* half the 0.37 s window */
    intro_print_free(&pr);
    free(z);
    CHECK(!intro_printer_open(1000));
    for (int rate = 8000; rate <= 96000; rate = rate == 8000 ? 11025 : rate == 11025 ? 22050 : rate == 22050 ? 44100 : rate * 2 + 7800) {
        Ep e = {rate, 10, 0, 0, 0, 0.01, 7, 0};
        CHECK(episode(&e, &pr));
        CHECK(abs(pr.n - 10 * INTRO_FPS) <= 7);
        intro_print_free(&pr);
    }
}

/* Two steady tones never make an intro (the browser tests' clips are tones): what changes from frame to frame in one
   is rounding - random bits -, and a print that comes out (nearly) all zeros or ones counts as silence. */
TEST(intro_steady_tones_match_nothing) {
    IntroPrint pr[2];
    for (int k = 0; k < 2; k++) {
        long n = 16000L * 40;
        float *x = malloc(sizeof(float) * (size_t)n);
        CHECK(x);
        for (long i = 0; i < n; i++) x[i] = (float)(0.3 * sin(2 * 3.14159265358979 * (440 + 330 * k) * i / 16000.0));
        IntroPrinter *p = intro_printer_open(16000);
        CHECK(p && intro_printer_feed(p, x, (int)n) && intro_printer_finish(p, &pr[k]));
        free(x);
    }
    IntroRun r[1];
    CHECK_INT(intro_match(&pr[0], &pr[1], 15 * INTRO_FPS, r, 1), 0);
    const IntroPrint *pp[2] = {&pr[0], &pr[1]};
    IntroFound f[2];
    intro_find(pp, 2, NULL, f);
    CHECK(f[0].from_ms < 0 && f[1].from_ms < 0);
    intro_print_free(&pr[0]);
    intro_print_free(&pr[1]);
}

TEST(intro_two_episodes_match_where_they_share) {
    IntroPrint a, b;
    Ep ea = {48000, 75, 31.5, 25, 1.0, 0.01, 101, 9};
    Ep eb = {44100, 60, 4.25, 25, 0.5, 0.03, 202, 9}; /* another rate, 6 dB softer, more noise */
    CHECK(episode(&ea, &a) && episode(&eb, &b));
    IntroRun runs[3];
    int n = intro_match(&a, &b, 15 * INTRO_FPS, runs, 3);
    CHECK(n >= 1);
    double af = a.start_ms / 1000.0 + runs[0].a_from / (double)INTRO_FPS, at = a.start_ms / 1000.0 + runs[0].a_to / (double)INTRO_FPS;
    double bf = b.start_ms / 1000.0 + runs[0].b_from / (double)INTRO_FPS;
    printf("  a %.2f-%.2f (want 31.5-56.5), b from %.2f (want 4.25), cost %d\n", af, at, bf, runs[0].cost);
    CHECK(fabs(af - 31.5) < 0.5 && fabs(at - 56.5) < 0.5 && fabs(bf - 4.25) < 0.5);
    CHECK(runs[0].cost < 900);
    intro_print_free(&a);
    intro_print_free(&b);
}

TEST(intro_a_season) {
    enum { N = 7 };
    /* the theme at various places, one episode (a special) without it */
    static const double at[N] = {0, 42.25, 15, 58.5, 0, 33, 50};
    IntroPrint pr[N];
    const IntroPrint *pp[N];
    for (int i = 0; i < N; i++) {
        Ep e = {i % 3 ? 11025 : 16000, 90, at[i], 21, i == 2 ? 0.7 : 1.0, 0.01 + 0.01 * (i % 2), 1000 + (uint32_t)i, i == 4 ? 0 : 77};
        CHECK(episode(&e, &pr[i]));
        pp[i] = &pr[i];
    }
    IntroFound f[N];
    intro_find(pp, N, NULL, f);
    for (int i = 0; i < N; i++) {
        printf("  ep %d: %.2f-%.2f votes %d (want %s)\n", i, f[i].from_ms / 1000.0, f[i].to_ms / 1000.0, f[i].votes, i == 4 ? "none" : "the theme");
        if (i == 4) {
            CHECK(f[i].from_ms < 0);
            continue;
        }
        CHECK(f[i].from_ms >= 0);
        CHECK(llabs(f[i].from_ms - (int64_t)(at[i] * 1000)) < 500);
        CHECK(llabs(f[i].to_ms - (int64_t)((at[i] + 21) * 1000)) < 500);
        CHECK(f[i].votes >= 2);
    }
    for (int i = 0; i < N; i++) intro_print_free(&pr[i]);
}

TEST(intro_nothing_shared_is_no_intro) {
    enum { N = 4 };
    IntroPrint pr[N];
    const IntroPrint *pp[N + 1];
    for (int i = 0; i < N; i++) {
        Ep e = {11025, 60, 0, 0, 0, 0.02, 500 + (uint32_t)i, 0};
        CHECK(episode(&e, &pr[i]));
        pp[i] = &pr[i];
    }
    pp[N] = NULL; /* one that couldn't be read */
    IntroFound f[N + 1];
    intro_find(pp, N + 1, NULL, f);
    for (int i = 0; i <= N; i++) CHECK(f[i].from_ms < 0);
    /* a theme too short to be an intro (an 8 s jingle) */
    for (int i = 0; i < N; i++) {
        intro_print_free(&pr[i]);
        Ep e = {11025, 60, 20 + i, 8, 1, 0.02, 600 + (uint32_t)i, 55};
        CHECK(episode(&e, &pr[i]));
    }
    intro_find(pp, N, NULL, f);
    for (int i = 0; i < N; i++) CHECK(f[i].from_ms < 0);
    /* two episodes are enough when that's all there is */
    for (int i = 0; i < 2; i++) {
        intro_print_free(&pr[i]);
        Ep e = {11025, 60, 10 + 20 * i, 20, 1, 0.02, 700 + (uint32_t)i, 56};
        CHECK(episode(&e, &pr[i]));
    }
    intro_find(pp, 2, NULL, f);
    CHECK(f[0].from_ms >= 0 && llabs(f[0].from_ms - 10000) < 500 && llabs(f[1].from_ms - 30000) < 500);
    for (int i = 0; i < N; i++) intro_print_free(&pr[i]);
}

static bool rd_file(void *ctx, int64_t off, void *dst, size_t n) { return file_pread((FILE *)ctx, off, dst, n); }

static int read_file(const char *path, int seconds, IntroPrint *out, char *why, size_t cap) {
    int64_t size = 0;
    FILE *f = path_stat(path, &size, NULL, NULL) ? file_open(path, "rb") : NULL;
    if (!f) return -99;
    MkvReader rd = {f, size, rd_file};
    IntroReadOpts o = {seconds, NULL, NULL};
    int rc = intro_read(&rd, &o, out, why, cap);
    fclose(f);
    return rc;
}

static bool stop_now(void *ctx) { return ++*(int *)ctx > 2; }

TEST(intro_read_mp4_files) {
    const char *dir = test_tmpdir();
    char a[1024], b[1024], c[1024];
    snprintf(a, sizeof a, "%s/a.mp4", dir);
    snprintf(b, sizeof b, "%s/b.mp4", dir);
    snprintf(c, sizeof c, "%s/c.txt", dir);
    CHECK(intro_test_episode(a, 45, 6, 18, 11, 99) && intro_test_episode(b, 40, 17.5, 18, 12, 99));
    IntroPrint pa, pb, pc;
    char why[200];
    CHECK_INT(read_file(a, 600, &pa, why, sizeof why), INTRO_READ_OK);
    CHECK_INT(read_file(b, 36, &pb, why, sizeof why), INTRO_READ_OK); /* (only its first 36 s: the theme ends at 35.5) */
    CHECK(abs(pa.n - 45 * INTRO_FPS) < INTRO_FPS && abs(pb.n - 36 * INTRO_FPS) < INTRO_FPS);
    IntroRun r[2];
    CHECK(intro_match(&pa, &pb, 15 * INTRO_FPS, r, 2) >= 1);
    double af = (pa.start_ms + r[0].a_from * 1000.0 / INTRO_FPS) / 1000, bf = (pb.start_ms + r[0].b_from * 1000.0 / INTRO_FPS) / 1000;
    printf("  a from %.2f (want 6), b from %.2f (want 17.5)\n", af, bf);
    CHECK(fabs(af - 6) < 0.6 && fabs(bf - 17.5) < 0.6);
    intro_print_free(&pa);
    intro_print_free(&pb);
    /* not a video; told to stop */
    FILE *t = fopen(c, "wb");
    CHECK(t && fputs("hello", t) >= 0 && fclose(t) == 0);
    CHECK_INT(read_file(c, 600, &pc, why, sizeof why), INTRO_READ_BROKEN);
    CHECK_HAS(why, "not a readable video");
    int64_t size = 0;
    CHECK(path_stat(a, &size, NULL, NULL));
    FILE *f = file_open(a, "rb");
    int calls = 0;
    MkvReader rd = {f, size, rd_file};
    IntroReadOpts o = {600, stop_now, &calls};
    CHECK_INT(intro_read(&rd, &o, &pc, why, sizeof why), INTRO_READ_STOPPED);
    CHECK(pc.n == 0 && !pc.v);
    fclose(f);
}

/* ---------------------------------------------- HEMMA_TEST_INTRO=<folder> */

typedef struct {
    char **v;
    int n, cap;
} Paths;

static void collect(const char *dir, const char *ext, Paths *ps, int depth) {
    DirIter *it = depth > 3 ? NULL : dir_open(dir);
    if (!it) return;
    DirEntry e;
    while (dir_next(it, &e)) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e.name);
        if (e.is_dir) {
            collect(p, ext, ps, depth + 1);
            continue;
        }
        size_t ln = strlen(p), le = strlen(ext);
        if (ln < le || strcmp(p + ln - le, ext)) continue;
        if (!strcmp(ext, ".mkv") || !strcmp(ext, ".mp4")) { /* .mp4 asked: not the .web.mp4 copies unless asked */
            if (ln > 8 && !strcmp(p + ln - 8, ".web.mp4") && strcmp(ext, ".web.mp4")) continue;
        }
        if (ps->n == ps->cap) ps->cap = ps->cap ? ps->cap * 2 : 64, ps->v = realloc(ps->v, sizeof(char *) * (size_t)ps->cap);
        ps->v[ps->n++] = strdup(p);
    }
    dir_close(it);
}

static int cmp_path(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }


TEST(intro_real_season_when_given) {
    const char *dir = getenv("HEMMA_TEST_INTRO");
    if (!dir || !*dir) return;
    const char *ext = getenv("HEMMA_TEST_INTRO_EXT") ? getenv("HEMMA_TEST_INTRO_EXT") : ".mkv";
    int secs = getenv("HEMMA_TEST_INTRO_S") ? atoi(getenv("HEMMA_TEST_INTRO_S")) : 600;
    Paths ps = {0};
    collect(dir, ext, &ps, 0);
    qsort(ps.v, (size_t)ps.n, sizeof(char *), cmp_path);
    CHECK(ps.n > 0);
    IntroPrint *pr = calloc((size_t)ps.n, sizeof *pr);
    const IntroPrint **pp = calloc((size_t)ps.n, sizeof *pp);
    int64_t t0 = now_ms();
    for (int i = 0; i < ps.n; i++) {
        int64_t size = 0, t = now_ms();
        FILE *f = path_stat(ps.v[i], &size, NULL, NULL) ? file_open(ps.v[i], "rb") : NULL;
        if (!f) continue;
        MkvReader rd = {f, size, rd_file};
        IntroReadOpts o = {secs, NULL, NULL};
        char why[200];
        int rc = intro_read(&rd, &o, &pr[i], why, sizeof why);
        fclose(f);
        if (rc == INTRO_READ_OK) pp[i] = &pr[i];
        printf("  read %s: %d frames, start %lld ms, %lld ms%s%s\n", strrchr(ps.v[i], '/') + 1, pr[i].n, (long long)pr[i].start_ms,
               (long long)(now_ms() - t), rc ? " - " : "", rc ? why : "");
    }
    int64_t t1 = now_ms();
    const char *pairs = getenv("HEMMA_TEST_INTRO_PAIRS"); /* "1": neighbours; "<k>" (1-based): episode k with every other */
    if (pairs)
        for (int i = 0; i < ps.n; i++)
            for (int j = i + 1; j < ps.n; j++) {
                int k = atoi(pairs) - 1;
                if (k > 0 ? (i != k && j != k) : j != i + 1) continue;
                if (!pp[i] || !pp[j]) continue;
                IntroRun runs[3];
                int nr = intro_match(pp[i], pp[j], 5 * INTRO_FPS, runs, 3);
                for (int r = 0; r < nr; r++)
                    printf("    %d~%d: %.1f-%.1f | %.1f-%.1f cost %.2f\n", i + 1, j + 1, runs[r].a_from / (double)INTRO_FPS,
                           runs[r].a_to / (double)INTRO_FPS, runs[r].b_from / (double)INTRO_FPS, runs[r].b_to / (double)INTRO_FPS,
                           runs[r].cost / 100.0);
            }
    IntroFound *f = calloc((size_t)ps.n, sizeof *f);
    intro_find(pp, ps.n, NULL, f);
    int found = 0;
    for (int i = 0; i < ps.n; i++) {
        found += f[i].from_ms >= 0;
        if (f[i].from_ms >= 0)
            printf("  %-60s intro %d:%05.2f - %d:%05.2f (%.1f s, %d votes)\n", strrchr(ps.v[i], '/') + 1, (int)(f[i].from_ms / 60000),
                   (f[i].from_ms % 60000) / 1000.0, (int)(f[i].to_ms / 60000), (f[i].to_ms % 60000) / 1000.0,
                   (f[i].to_ms - f[i].from_ms) / 1000.0, f[i].votes);
        else printf("  %-60s no intro\n", strrchr(ps.v[i], '/') + 1);
    }
    printf("intro: %d of %d episodes; reading %.1f s, matching %.1f s\n", found, ps.n, (t1 - t0) / 1000.0, (now_ms() - t1) / 1000.0);
    for (int i = 0; i < ps.n; i++) intro_print_free(&pr[i]), free(ps.v[i]);
    free(ps.v);
    free(pr);
    free(pp);
    free(f);
}
