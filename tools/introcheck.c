/* tools/introcheck.c — dev aid for modules/av/intro (2.9): what Hemma's intro finding makes of a season, and how long it
 * takes on this computer - a Raspberry Pi's, say - without Hemma: each file's sound read and fingerprinted, then the
 * season matched.
 *
 *   introcheck [-s SECONDS] FILE...          (the episodes in order; SECONDS of each read, 600 by default)
 *
 *   zig cc -O2 -std=gnu11 -target aarch64-linux-musl tools/introcheck.c modules/av/intro.c modules/av/aacdec.c \
 *          modules/av/aac.c modules/av/ac3.c modules/av/dts.c modules/av/mkv.c -lm -o build/introcheck-pi64
 *   (on Windows: the same without -target, plus -D_FILE_OFFSET_BITS=64; any Linux: cc ... -lm)
 */
#define _FILE_OFFSET_BITS 64
#include "../modules/av/intro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static bool rd(void *ctx, int64_t off, void *dst, size_t n) {
    FILE *f = ctx;
#ifdef _WIN32
    if (_fseeki64(f, off, SEEK_SET)) return false;
#else
    if (fseeko(f, (off_t)off, SEEK_SET)) return false;
#endif
    return fread(dst, 1, n, f) == n;
}

static const char *base(const char *p) {
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > s) s = b;
    return s ? s + 1 : p;
}

int main(int argc, char **argv) {
    int seconds = 600, first = 1;
    if (argc > 2 && !strcmp(argv[1], "-s")) seconds = atoi(argv[2]), first = 3;
    int n = argc - first;
    if (n < 1) {
        fprintf(stderr, "usage: introcheck [-s SECONDS] FILE...\n");
        return 2;
    }
    IntroPrint *pr = calloc((size_t)n, sizeof *pr);
    const IntroPrint **pp = calloc((size_t)n, sizeof *pp);
    IntroFound *found = calloc((size_t)n, sizeof *found);
    if (!pr || !pp || !found) return 1;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        const char *path = argv[first + i];
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("%-50s can't be opened\n", base(path));
            continue;
        }
        fseek(f, 0, SEEK_END);
#ifdef _WIN32
        int64_t size = _ftelli64(f);
#else
        int64_t size = (int64_t)ftello(f);
#endif
        MkvReader r = {f, size, rd};
        IntroReadOpts o = {seconds, NULL, NULL};
        char why[200] = "";
        double t = now_s();
        int rc = intro_read(&r, &o, &pr[i], why, sizeof why);
        fclose(f);
        if (rc == INTRO_READ_OK) pp[i] = &pr[i];
        printf("%-50s %5.1f s  %5d frames%s%s\n", base(path), now_s() - t, pr[i].n, rc ? "  - " : "", rc ? why : "");
        fflush(stdout);
    }
    double t1 = now_s();
    intro_find(pp, n, NULL, found);
    double t2 = now_s();
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (found[i].from_ms < 0) {
            printf("%-50s no intro\n", base(argv[first + i]));
            continue;
        }
        k++;
        printf("%-50s intro %lld:%05.2f - %lld:%05.2f  (%.1f s, %d votes)\n", base(argv[first + i]), (long long)(found[i].from_ms / 60000),
               (found[i].from_ms % 60000) / 1000.0, (long long)(found[i].to_ms / 60000), (found[i].to_ms % 60000) / 1000.0,
               (found[i].to_ms - found[i].from_ms) / 1000.0, found[i].votes);
    }
    printf("%d of %d with an intro; reading %.1f s (%.1f s a file), matching %.2f s\n", k, n, t1 - t0, (t1 - t0) / n, t2 - t1);
    for (int i = 0; i < n; i++) intro_print_free(&pr[i]);
    free(pr);
    free(pp);
    free(found);
    return 0;
}
