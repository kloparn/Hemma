/* The version is the VERSION file, and CHANGELOG.md's newest section is that version: a release is
 * refused without its section (.github/workflows/release.yml), so a PR that raises the version
 * without one fails here first. Run from the top of the repo, as every runner does. */
#include "../tests/test.h"
#include "app.h"
#include "plat.h"

TEST(version_comes_from_the_version_file) {
    Buf v = {0};
    CHECK(file_read_all("VERSION", &v, 64));
    char want[64];
    snprintf(want, sizeof want, "%s", v.p ? v.p : "");
    want[strcspn(want, " \r\n")] = '\0';
    CHECK_STR(hemma_version(), want);
    CHECK(strncmp(HEMMA_VERSION_FULL, want, strlen(want)) == 0);
    buf_free(&v);
}

TEST(version_has_the_newest_changelog_section) {
    Buf log = {0};
    CHECK(file_read_all("CHANGELOG.md", &log, 4 << 20));
    const char *h = log.p ? log.p : "";
    while (*h && strncmp(h, "## ", 3) != 0) { /* the first line that starts a section */
        const char *nl = strchr(h, '\n');
        h = nl ? nl + 1 : h + strlen(h);
    }
    char want[80];
    snprintf(want, sizeof want, "## %s (", hemma_version());
    CHECK(strncmp(h, want, strlen(want)) == 0);
    buf_free(&log);
}
