/* tests/runner.c — runs every TEST() linked into the binary.
 *
 *   build/test                run everything
 *   build/test http ws        run tests whose name or file contains "http" or "ws"
 *   build/test -v             list each test as it runs
 *   build/test --shard K/N    run every Nth of them, from the Kth (0-based), and end with one line
 *                             "shard K/N: R run, F failed, C checks, M ms": N of these side by side
 *                             (tests/parallel.sh, build.ps1) keep every core busy. Each has its own
 *                             scratch folder, as any run does. */
#include "test.h"
#include "../core/plat.h"

typedef struct {
    const char *name, *file;
    TestFn fn;
} Test;

static Test *tests;
static int ntests, captests;
static int failed_in_current;
static const char *current_name;
int test_checks;

void test_register(const char *name, const char *file, TestFn fn) {
    if (ntests == captests) {
        captests = captests ? captests * 2 : 64;
        tests = xrealloc(tests, (size_t)captests * sizeof *tests);
    }
    tests[ntests++] = (Test){name, file, fn};
}

void test_fail(const char *file, int line, const char *fmt, ...) {
    failed_in_current++;
    fprintf(stderr, "  \x1b[31mFAIL\x1b[0m %s\n    %s:%d: ", current_name, file, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int tmp_seq;
static char tmp_root[256];

const char *test_tmpdir(void) {
    static char path[512];
    snprintf(path, sizeof path, "%s/t%d", tmp_root, ++tmp_seq);
    dir_make(path);
    return path;
}

/* Everything under path (a test run's own scratch folder), bounded in depth. */
static void remove_tree(const char *path, int depth) {
    DirIter *it = depth < 16 ? dir_open(path) : NULL;
    DirEntry e;
    while (it && dir_next(it, &e)) {
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e.name);
        if (e.is_dir) remove_tree(sub, depth + 1);
        else file_remove(sub);
    }
    if (it) dir_close(it);
    dir_remove(path);
}

static int by_file_then_name(const void *a, const void *b) {
    const Test *x = a, *y = b;
    int c = strcmp(x->file, y->file);
    return c ? c : strcmp(x->name, y->name);
}

static const char *short_file(const char *f) {
    const char *s = f;
    for (const char *p = f; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    return s;
}

int main(int argc, char **argv) {
    plat_init();
    g_log_level = LOG_QUIET;
    bool verbose = false;
    const char *filters[32];
    int nfilters = 0, shard = 0, shards = 1;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "-v")) verbose = true;
        else if (str_eq(argv[i], "--shard") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d/%d", &shard, &shards) != 2 || shards < 1 || shard < 0 || shard >= shards) {
                fprintf(stderr, "--shard K/N: 0 <= K < N\n");
                return 2;
            }
        } else if (nfilters < countof(filters)) filters[nfilters++] = argv[i];
    }

    /* scratch files go next to the test program (its own build folder) and are removed after a
       passing run; a failing run keeps them to look at */
    char rnd[9], exe_dir[200] = ".";
    rand_hex(rnd, 4);
    const char *slash = strrchr(argv[0], '/'), *back = strrchr(argv[0], '\\');
    if (back > slash) slash = back;
    if (slash && slash - argv[0] < (int)sizeof exe_dir) snprintf(exe_dir, sizeof exe_dir, "%.*s", (int)(slash - argv[0]), argv[0]);
    const char *dir = exe_dir; /* "./build" -> "build": tests compare paths the library normalises */
    while (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\\')) dir += 2;
    snprintf(tmp_root, sizeof tmp_root, "%s/test-tmp/%s", str_eq(dir, ".") ? "." : dir, rnd);
    for (char *p = tmp_root; *p; p++) /* "/" only, as the library writes paths */
        if (*p == '\\') *p = '/';
    dir_make(tmp_root);

    qsort(tests, (size_t)ntests, sizeof *tests, by_file_then_name);
    int run = 0, failed = 0, picked = 0;
    int64_t t0 = now_ms();
    for (int i = 0; i < ntests; i++) {
        Test *t = &tests[i];
        if (nfilters) {
            bool hit = false;
            for (int k = 0; k < nfilters && !hit; k++)
                hit = strstr(t->name, filters[k]) || strstr(t->file, filters[k]);
            if (!hit) continue;
        }
        if (picked++ % shards != shard) continue; /* (a file's tests spread over the shards: even loads) */
        current_name = t->name;
        failed_in_current = 0;
        g_fake_now = 0;
        if (verbose) fprintf(stderr, "  %-28s %s\n", short_file(t->file), t->name);
        t->fn();
        run++;
        if (failed_in_current) failed++;
    }
    int64_t ms = now_ms() - t0;
    if (shards > 1)
        fprintf(stderr, "shard %d/%d: %d run, %d failed, %d checks, %lld ms\n", shard, shards, run, failed,
                test_checks, (long long)ms);
    else if (failed)
        fprintf(stderr, "\n\x1b[31m%d of %d tests failed\x1b[0m (%d checks, %lld ms)\n", failed, run, test_checks,
                (long long)ms);
    else
        fprintf(stderr, "\n\x1b[32mall %d tests passed\x1b[0m (%d checks, %lld ms)\n", run, test_checks, (long long)ms);
    if (!failed) remove_tree(tmp_root, 0);
    else fprintf(stderr, "test files kept in %s\n", tmp_root);
    return failed ? 1 : 0;
}
