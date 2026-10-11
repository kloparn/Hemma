#include "../tests/test.h"
#include "env.h"
#include "plat.h"

static Buf seen;
static void collect(const char *k, const char *v, void *ctx) { buf_printf(&seen, "%s=[%s];", k, v); }

TEST(env_parses_dotenv_syntax) {
    buf_reset(&seen);
    const char *t = "\xEF\xBB\xBF# comment\n\nexport A=1\nB = spaced value  # trailing comment\n"
                    "C=\"quoted # not a comment\\n\\\"x\\\"\"\nD='single $x'\r\nE=\nF=a#b\n";
    CHECK_INT(env_parse(t, collect, NULL), 6);
    CHECK_STR(buf_cstr(&seen),
              "A=[1];B=[spaced value];C=[quoted # not a comment\n\"x\"];D=[single $x];E=[];F=[a#b];");
}

TEST(env_reports_the_first_bad_line) {
    buf_reset(&seen);
    CHECK_INT(env_parse("1BAD=x\n", collect, NULL), -1);
    CHECK_INT(env_parse("A=1\nNO_EQUALS\n", collect, NULL), -2);
    CHECK_INT(env_parse("A=\"unterminated\n", collect, NULL), -1);
    CHECK_INT(env_parse("A='open\n", collect, NULL), -1);
    CHECK_INT(env_parse("=x\n", collect, NULL), -1);
    CHECK_INT(env_parse("", collect, NULL), 0);
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
