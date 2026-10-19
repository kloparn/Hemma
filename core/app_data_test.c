/* The data folder as the whole program sees it, every module loaded: each table says what it holds
 * (a rescue reports in those words), renames come with the migration that keeps older programs off
 * them, and a start refused over the data says so in hemma.log and exits with its own code. */
#include "../tests/harness.h"
#include "plat.h"

/* Issue #88: "rows were lost in: board_note" told the household nothing. */
TEST(app_every_table_says_what_it_holds) {
    th_boot();
    Table **ts;
    int n = store_tables(&ts);
    CHECK(n > 10);
    Buf missing = {0};
    for (int i = 0; i < n; i++)
        if (!ts[i]->memory_only && !(ts[i]->what && ts[i]->what[0])) buf_printf(&missing, " %s", ts[i]->name);
    CHECK_STR(missing.p ? missing.p : "", ""); /* (set Table.what in the module's init) */
    buf_free(&missing);
    th_halt();
}

/* Issue #92: a FIELD_WAS without a breaks_older migration at or after its format lets an older
   Hemma re-add the old column, empty - and show those rows blank. */
TEST(app_every_rename_keeps_older_programs_off_it) {
    th_boot();
    const char *bad = store_renames_unsafe();
    CHECK_STR(bad ? bad : "", ""); /* (add a breaks_older migration in core/store.c's MIGRATIONS) */
    th_halt();
}

/* Issue #92: a refused start went to the journal only, and systemd restarted it every 2 s. */
TEST(app_a_start_refused_over_the_data_says_so_in_the_log_file) {
    const char *dir = test_tmpdir();
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    Buf junk = {0};
    for (int i = 0; i < 200; i++) buf_puts(&junk, "an SD card after a power cut. ");
    CHECK(file_write_atomic(path, junk.p, junk.len));
    app_set_testing(true);
    AppConfig cfg = {.data_dir = dir}; /* (no host or port: it never gets that far) */
    CHECK_INT(app_main(&cfg), HEMMA_EXIT_DATA);
    store_close();
    snprintf(path, sizeof path, "%s/hemma.log", dir);
    Buf log = {0};
    CHECK(file_read_all(path, &log, 1 << 20));
    char started[64];
    snprintf(started, sizeof started, " - Hemma %s", HEMMA_VERSION);
    CHECK_HAS(log.p, started);
    CHECK_HAS(log.p, " started\n");
    CHECK_HAS(log.p, " E Hemma's data in ");
    CHECK_HAS(log.p, "a rescue got nothing back (0 rows). Hemma won't start on it and changed nothing");
    CHECK_HAS(log.p, " - Hemma stopped\n"); /* (a start with its stop: the System page sees no crash) */
    buf_free(&log);
    buf_free(&junk);
}
