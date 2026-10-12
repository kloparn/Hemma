#include "../tests/test.h"
#include "env.h"
#include "plat.h"

static Buf seen;
static void collect(const char *k, const char *v, int line, void *ctx) {
    (void)ctx;
    buf_printf(&seen, "%s=[%s]@%d;", k, v, line);
}

TEST(env_parses_dotenv_syntax) {
    buf_reset(&seen);
    const char *t = "\xEF\xBB\xBF# comment\n\nexport A=1\nB = spaced value  # trailing comment\n"
                    "C=\"quoted # not a comment\\n\\\"x\\\"\"\nD='single $x'\r\nE=\nF=a#b\n";
    int bad = -1;
    CHECK_INT(env_parse(t, collect, NULL, &bad), 6);
    CHECK_INT(bad, 0);
    CHECK_STR(buf_cstr(&seen),
              "A=[1]@3;B=[spaced value]@4;C=[quoted # not a comment\n\"x\"]@5;D=[single $x]@6;E=[]@7;F=[a#b]@8;");
}

TEST(env_comments_after_an_empty_value) {
    buf_reset(&seen);
    /* issue #12: the leading spaces were skipped first, so " #" no longer matched */
    CHECK_INT(env_parse("A= # comment\nB=\t# tab comment\nC=#not-a-comment\nD=x\t# tab\n", collect, NULL, NULL), 4);
    CHECK_STR(buf_cstr(&seen), "A=[]@1;B=[]@2;C=[#not-a-comment]@3;D=[x]@4;");
    buf_reset(&seen);
    CHECK_INT(env_parse("\xEF\xBB\xBFHEMMA_AUTH_SALT=\n", collect, NULL, NULL), 1); /* a BOM before the key */
    CHECK_STR(buf_cstr(&seen), "HEMMA_AUTH_SALT=[]@1;");
}

TEST(env_skips_bad_lines_and_reads_on) {
    int bad = 0;
    buf_reset(&seen);
    CHECK_INT(env_parse("1BAD=x\n", collect, NULL, &bad), 0);
    CHECK_INT(bad, 1);
    CHECK_INT(env_parse("A=1\nNO_EQUALS\nB=2\n", collect, NULL, &bad), 2);
    CHECK_INT(bad, 2);
    CHECK_INT(env_parse("A=\"unterminated\nB=2\n", collect, NULL, &bad), 1);
    CHECK_INT(bad, 1);
    CHECK_INT(env_parse("A='open\n", collect, NULL, &bad), 0);
    CHECK_INT(bad, 1);
    CHECK_INT(env_parse("=x\nok=1\nX Y=2\n", collect, NULL, &bad), 1);
    CHECK_INT(bad, 1);
    CHECK_INT(env_parse("", collect, NULL, &bad), 0);
    CHECK_INT(bad, 0);
    CHECK_STR(buf_cstr(&seen), "A=[1]@1;B=[2]@3;B=[2]@2;ok=[1]@2;");
}

TEST(env_line_key_finds_the_key_a_line_sets) {
    char k[64];
    CHECK(env_line_key("HEMMA_AUTH_SALT=", k, sizeof k));
    CHECK_STR(k, "HEMMA_AUTH_SALT");
    CHECK(env_line_key("\xEF\xBB\xBFHEMMA_AUTH_SALT=\r", k, sizeof k));
    CHECK_STR(k, "HEMMA_AUTH_SALT");
    CHECK(env_line_key("  export  HEMMA_AUTH_SALT = # empty", k, sizeof k));
    CHECK_STR(k, "HEMMA_AUTH_SALT");
    CHECK(!env_line_key("# HEMMA_AUTH_SALT=x", k, sizeof k));
    CHECK(!env_line_key("", k, sizeof k));
    CHECK(!env_line_key("HEMMA AUTH=x", k, sizeof k));
    CHECK(!env_line_key("TOO_LONG_FOR_THE_BUFFER=x", k, 8));
}

TEST(env_file_never_overrides_the_real_environment) {
    char path[600];
    snprintf(path, sizeof path, "%s/.env", test_tmpdir());
    const char *text = "HEMMA_TEST_X=from-file\nHEMMA_TEST_Y=from-file\n";
    CHECK(file_write_atomic(path, text, strlen(text)));
    CHECK(plat_setenv("HEMMA_TEST_X", "real", true));
    CHECK_INT(env_load(path), 2);
    CHECK_STR(env("HEMMA_TEST_X", NULL), "real");
    CHECK_STR(env("HEMMA_TEST_Y", NULL), "from-file");
    CHECK_INT(env_load("no/such/.env"), -1);
    CHECK_STR(env("HEMMA_TEST_MISSING", "dflt"), "dflt");
    plat_setenv("HEMMA_TEST_N", "42", true);
    CHECK_INT(env_int("HEMMA_TEST_N", 0), 42);
    plat_setenv("HEMMA_TEST_X", "", true);
    plat_setenv("HEMMA_TEST_Y", "", true);
    plat_setenv("HEMMA_TEST_N", "", true);
    CHECK(env("HEMMA_TEST_X", NULL) == NULL); /* empty counts as unset */
}

/* Issue #12: a BOM + "HEMMA_AUTH_SALT=" on line 1 and the real secret further down (or a bad
   line in between) locked everyone out. The first value counts; an empty one doesn't. */
TEST(env_an_empty_or_broken_line_never_hides_a_later_value) {
    char path[600];
    snprintf(path, sizeof path, "%s/.env", test_tmpdir());
    const char *text = "\xEF\xBB\xBFHEMMA_TEST_S=\nnot a setting\nHEMMA_TEST_S=secret\nHEMMA_TEST_S=other\n"
                       "HEMMA_TEST_T=after-the-bad-line\n";
    CHECK(file_write_atomic(path, text, strlen(text)));
    plat_setenv("HEMMA_TEST_S", "", true);
    plat_setenv("HEMMA_TEST_T", "", true);
    CHECK_INT(env_load(path), 4);
    CHECK_STR(env("HEMMA_TEST_S", NULL), "secret");
    CHECK_STR(env("HEMMA_TEST_T", NULL), "after-the-bad-line");
    plat_setenv("HEMMA_TEST_S", "", true);
    plat_setenv("HEMMA_TEST_T", "", true);
}
