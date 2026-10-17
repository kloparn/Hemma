#include "../tests/test.h"
#include "db.h"
#include "plat.h"
#include "store.h"

typedef struct {
    int id;
    char name[16];
    int64_t when;
    bool done;
    uint64_t hash;
} Thing;
static const FieldDef thing_fields[] = {FIELD(Thing, id, F_INT), FIELD(Thing, name, F_STR), FIELD(Thing, when, F_I64),
                                        FIELD(Thing, done, F_BOOL), FIELD(Thing, hash, F_U64)};

TEST(store_put_get_delete_and_reopen) {
    const char *dir = test_tmpdir();
    Table t = TABLE("thing", Thing, thing_fields);
    store_register(&t);
    CHECK(store_open(dir));
    Thing a = {0};
    str_copy(a.name, sizeof a.name, "Bilen åäö");
    a.when = 1760000000;
    a.done = true;
    a.hash = 0xfedcba9876543210ULL;
    Thing *row = tbl_put(&t, &a);
    CHECK_INT(a.id, 1);
    CHECK_INT(row->id, 1);
    Thing b = {0};
    str_copy(b.name, sizeof b.name, "two");
    tbl_put(&t, &b);
    CHECK_INT(b.id, 2);
    CHECK_INT(tbl_count(&t), 2);
    Thing upd = *(Thing *)tbl_get(&t, 1);
    upd.done = false;
    tbl_put(&t, &upd);
    CHECK(tbl_del(&t, 2));
    CHECK(!tbl_del(&t, 2));
    CHECK(tbl_get(&t, 2) == NULL);
    store_close();

    Table t2 = TABLE("thing", Thing, thing_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t2), 1);
    Thing *r = tbl_get(&t2, 1);
    CHECK(r != NULL);
    CHECK_STR(r->name, "Bilen åäö");
    CHECK(r->when == 1760000000 && !r->done && r->hash == 0xfedcba9876543210ULL);
    Thing c = {0};
    tbl_put(&t2, &c);
    CHECK_INT(c.id, 3); /* AUTOINCREMENT: the deleted id 2 is never reused */
    CHECK(!store_failed());
    store_close();
}

typedef struct {
    int id;
    char name[16];
} V1;
typedef struct {
    int id;
    char name[16];
    int prio;
    char note[8];
} V2;
static const FieldDef v1_fields[] = {FIELD(V1, id, F_INT), FIELD(V1, name, F_STR)};
static const FieldDef v2_fields[] = {FIELD(V2, id, F_INT), FIELD(V2, name, F_STR), FIELD(V2, prio, F_INT),
                                     FIELD(V2, note, F_STR)};

TEST(store_adds_columns_for_new_fields) {
    const char *dir = test_tmpdir();
    Table a = TABLE("evolve", V1, v1_fields);
    store_register(&a);
    CHECK(store_open(dir));
    V1 one = {0};
    str_copy(one.name, sizeof one.name, "old row");
    tbl_put(&a, &one);
    store_close();

    Table b = TABLE("evolve", V2, v2_fields);
    store_register(&b);
    CHECK(store_open(dir));
    V2 *r = tbl_get(&b, 1);
    CHECK(r != NULL);
    CHECK_STR(r->name, "old row");
    CHECK_INT(r->prio, 0);
    CHECK_STR(r->note, "");
    V2 upd = *r;
    upd.prio = 7;
    str_copy(upd.note, sizeof upd.note, "hi");
    tbl_put(&b, &upd);
    store_close();

    Table c = TABLE("evolve", V2, v2_fields);
    store_register(&c);
    CHECK(store_open(dir));
    r = tbl_get(&c, 1);
    CHECK(r && r->prio == 7);
    CHECK_STR(r->note, "hi");
    store_close();
}

TEST(store_truncates_long_text_safely_on_load) {
    const char *dir = test_tmpdir();
    Table t = TABLE("thing", Thing, thing_fields);
    store_register(&t);
    CHECK(store_open(dir));
    /* written by someone else (a newer version, a SQL tool) with more room */
    CHECK(db_exec("INSERT INTO thing (name) VALUES ('åäöåäöåäöåäö-too-long')"));
    store_close();
    Table t2 = TABLE("thing", Thing, thing_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    Thing *r = tbl_get(&t2, 1);
    CHECK(r != NULL);
    CHECK_INT(strlen(r->name), 14); /* 7 two-byte letters fit in 15 bytes + NUL */
    CHECK(utf8_valid(r->name, strlen(r->name)));
    store_close();
}

TEST(store_memory_only_tables_stay_in_memory) {
    const char *dir = test_tmpdir();
    Table t = TABLE("cache", Thing, thing_fields);
    t.memory_only = true;
    store_register(&t);
    CHECK(store_open(dir));
    Thing a = {0};
    tbl_put(&t, &a);
    tbl_put(&t, &a); /* same id again: an update */
    Thing b = {0};
    tbl_put(&t, &b);
    CHECK_INT(tbl_count(&t), 2);
    CHECK_INT(b.id, 2);
    sqlite3_stmt *st = db_prep("SELECT count(*) FROM sqlite_master WHERE name = 'cache'");
    CHECK(st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    store_close();
}

TEST(store_rejects_bad_table_definitions) {
    const char *dir = test_tmpdir();
    Table bad = TABLE("Bad Name", Thing, thing_fields);
    store_register(&bad);
    CHECK(store_table("Bad Name") == NULL);
    static const FieldDef no_id[] = {FIELD(Thing, name, F_STR)};
    Table t2 = TABLE("noid", Thing, no_id);
    store_register(&t2);
    CHECK(store_table("noid") == NULL);
    CHECK(store_open(dir));
    store_close();
}

TEST(store_raw_sql_and_nested_transactions) {
    const char *dir = test_tmpdir();
    Table t = TABLE("thing", Thing, thing_fields);
    store_register(&t);
    CHECK(store_open(dir));
    db_begin();
    db_begin(); /* nested: only the outer pair is real */
    for (int i = 0; i < 50; i++) {
        Thing x = {0};
        x.when = i;
        tbl_put(&t, &x);
    }
    db_commit();
    CHECK(sqlite3_get_autocommit(db()) == 0); /* still inside the outer transaction */
    db_commit();
    CHECK(sqlite3_get_autocommit(db()) != 0);
    sqlite3_stmt *st = db_prep("SELECT count(*), sum(\"when\") FROM thing WHERE \"when\" >= ?");
    CHECK(st != NULL);
    sqlite3_bind_int(st, 1, 10);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 40);
    CHECK_INT(sqlite3_column_int(st, 1), 1180);
    /* a cached statement comes back reset and unbound */
    st = db_prep("SELECT count(*), sum(\"when\") FROM thing WHERE \"when\" >= ?");
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECK_INT(sqlite3_column_int(st, 0), 0); /* NULL bound: nothing matches */
    db_idle();
    store_maintain();
    CHECK(db_prep("SELEKT nonsense") == NULL);
    store_close();
}

TEST(store_rows_encode_as_json) {
    Table t = TABLE("thing", Thing, thing_fields);
    Thing a = {.id = 4, .when = -5, .done = true, .hash = 255};
    str_copy(a.name, sizeof a.name, "say \"hi\"");
    Buf b = {0};
    tbl_to_json(&t, &a, &b);
    CHECK_STR(buf_cstr(&b), "{\"id\":4,\"name\":\"say \\\"hi\\\"\",\"when\":-5,\"done\":true,\"hash\":\"00000000000000ff\"}");
    buf_free(&b);
}

/* ===================================================== damaged files (issue #37) */

typedef struct {
    int id;
    char text[40];
} Note;
static const FieldDef note_fields[] = {FIELD(Note, id, F_INT), FIELD(Note, text, F_STR)};

/* The newest kept copy of a damaged file in dir (names carry the day and time): its size, or -1 when
   there's none. (By name, not the listing's order: NTFS lists by name, ext4 and APFS don't.) */
static int64_t damaged_copy(const char *dir) {
    DirIter *it = dir_open(dir);
    DirEntry e;
    int64_t size = -1;
    char newest[256] = "";
    while (it && dir_next(it, &e))
        if (str_starts(e.name, "hemma.db.damaged-") && !str_ends(e.name, "-wal") && strcmp(e.name, newest) > 0) {
            str_copy(newest, sizeof newest, e.name);
            size = e.size;
        }
    dir_close(it);
    return size;
}

/* A data folder with `notes` notes (and `people` people, when > 0), closed cleanly - so the census
   (hemma.db.counts) knows them - and its file read into f. */
typedef struct {
    int id;
    char name[24];
} Who;
static const FieldDef who_fields[] = {FIELD(Who, id, F_INT), FIELD(Who, name, F_STR)};

static void filled(const char *dir, int notes, int people, Buf *f) {
    Table t = TABLE("note", Note, note_fields), p = TABLE("person", Who, who_fields);
    store_register(&t);
    if (people) store_register(&p);
    CHECK(store_open(dir));
    db_begin();
    for (int i = 1; i <= notes; i++) {
        Note n = {0};
        snprintf(n.text, sizeof n.text, "note-%05d-on-the-fridge", i);
        tbl_put(&t, &n);
    }
    for (int i = 1; i <= people; i++) {
        Who w = {0};
        snprintf(w.name, sizeof w.name, "person-%d-of-the-house", i);
        tbl_put(&p, &w);
    }
    db_commit();
    store_close(); /* (the last connection: the WAL goes into the file; the census is written) */
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    buf_reset(f);
    CHECK(file_read_all(path, f, 64u << 20));
}

static void put_file(const char *dir, const Buf *f, size_t len) {
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    CHECK(file_write_atomic(path, f->p, len));
}

/* The page that holds `text` (the file's first one that does): false = none does. */
static bool page_of(const Buf *f, const char *text, size_t *pg) {
    size_t n = strlen(text), at = 0;
    while (at + n <= f->len && memcmp(f->p + at, text, n) != 0) at++;
    *pg = at / 4096 * 4096;
    return at + n <= f->len;
}

TEST(store_rescues_what_a_damaged_database_still_holds) {
    /* issue #37: one damaged page in 3,000 notes, and Hemma started with 1,622 of them - the loader
       took the damage for the end of the table - with nothing but "malformed" in the log */
    const char *dir = test_tmpdir();
    Buf f = {0};
    filled(dir, 3000, 0, &f);
    /* the page holding note 1500 turns to rubbish, as an SD card does after a power cut */
    size_t pg = 0;
    CHECK(page_of(&f, "note-01500-", &pg));
    memset(f.p + pg, 0xA5, 4096);
    put_file(dir, &f, f.len);

    Table t2 = TABLE("note", Note, note_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    int n = tbl_count(&t2);
    CHECK(n >= 2860 && n < 3000);          /* one page of notes lost (about 130 fit on one) ... */
    CHECK(tbl_get(&t2, 1500) == NULL);
    CHECK(tbl_get(&t2, 3000) != NULL);     /* ... and everything after it kept */
    /* in the household's words, against the census (issue #88): not a table name and no count */
    CHECK_HAS(store_rescue_report(), "Hemma's data was damaged (parts of it couldn't be read). Nothing was deleted: "
                                     "the damaged file is kept as hemma.db.damaged-");
    char want[80];
    snprintf(want, sizeof want, "%d,%03d of 3,000 rows were kept - note: %d,%03d of 3,000.", n / 1000, n % 1000,
             n / 1000, n % 1000);
    CHECK_HAS(store_rescue_report(), want);
    CHECK_INT(damaged_copy(dir), (int)f.len); /* the damaged file, kept as it was */
    Note x = {0};
    tbl_put(&t2, &x);
    CHECK(x.id > 3000); /* a lost note's id is never handed out again */
    CHECK(!store_failed());
    store_close();
    /* the next start is an ordinary one */
    Table t3 = TABLE("note", Note, note_fields);
    store_register(&t3);
    CHECK(store_open(dir));
    CHECK_STR(store_rescue_report(), "");
    CHECK_INT(tbl_count(&t3), n + 1);
    store_close();
    buf_free(&f);
}

/* Issue #88: damage on the page that points to the notes' pages lost every one of them - a SELECT
   stops at the first broken page of the tree; SQLite's recovery reads the pages under it anyway. */
TEST(store_rescue_keeps_rows_under_a_damaged_tree_page) {
    const char *dir = test_tmpdir();
    Buf f = {0};
    filled(dir, 3000, 0, &f);
    size_t interior = 0;
    for (size_t pg = 4096; pg + 4096 <= f.len && !interior; pg += 4096)
        if ((unsigned char)f.p[pg] == 0x05) interior = pg; /* (an interior page of a table's tree) */
    CHECK(interior > 0);
    memset(f.p + interior, 0xA5, 4096);
    put_file(dir, &f, f.len);
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t), 3000);
    CHECK_HAS(store_rescue_report(), "All 3,000 rows were kept.");
    store_close();
    buf_free(&f);
}

/* Raw tables' rows (a cache Hemma rebuilds) under damaged tree pages: they stay in lost_and_found and
   never go into the household's tables. A cache shaped unlike any of ours is only logged; one shaped
   like the notes might have been notes, so the report counts it - but it's never put among them. */
static void raw_table(const char *name, const char *columns, const char *insert, int n, long long *root) {
    char sql[300];
    snprintf(sql, sizeof sql, "CREATE TABLE %s (%s)", name, columns);
    CHECK(db_exec(sql));
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db(), insert, -1, &st, NULL) == SQLITE_OK);
    for (int i = 1; i <= n; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, i);
        CHECK(sqlite3_step(st) == SQLITE_DONE);
    }
    sqlite3_finalize(st);
    *root = 0;
    CHECK(sqlite3_prepare_v2(db(), "SELECT rootpage FROM sqlite_master WHERE name = ?1", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) *root = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
}

TEST(store_rescue_keeps_a_caches_rows_out_of_the_households) {
    const char *dir = test_tmpdir();
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(store_open(dir));
    db_begin();
    for (int i = 1; i <= 300; i++) {
        Note n = {0};
        snprintf(n.text, sizeof n.text, "note-%05d-on-the-fridge", i);
        tbl_put(&t, &n);
    }
    long long roots[2];
    raw_table("cache", "id INTEGER PRIMARY KEY, path TEXT NOT NULL, size INTEGER NOT NULL",
              "INSERT INTO cache (path, size) VALUES (printf('cached/%05d/some/file.mkv', ?1), ?1 * 1000)", 2000, &roots[0]);
    raw_table("twin", "id INTEGER PRIMARY KEY, text TEXT NOT NULL",
              "INSERT INTO twin (text) VALUES (printf('twin-%05d-looks-like-a-note', ?1))", 2000, &roots[1]);
    db_commit();
    store_close();
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    Buf f = {0};
    CHECK(file_read_all(path, &f, 64u << 20));
    for (int i = 0; i < 2; i++) {
        size_t at = (size_t)(roots[i] - 1) * 4096;
        CHECK(roots[i] > 1 && at + 4096 <= f.len && (unsigned char)f.p[at] == 0x05); /* (an interior page) */
        memset(f.p + at, 0xA5, 4096);
    }
    put_file(dir, &f, f.len);

    Table t2 = TABLE("note", Note, note_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t2), 300); /* (none of the twin's rows among the notes) */
    const char *r = store_rescue_report(), *more = strstr(r, "All 300 rows were kept. ");
    CHECK(more != NULL);
    long long unsure = 0;
    for (const char *p = more ? more + 24 : ""; (*p >= '0' && *p <= '9') || *p == ','; p++)
        if (*p != ',') unsure = unsure * 10 + (*p - '0');
    CHECK(unsure > 1000 && unsure <= 2000); /* the twin's rows that could have been notes - not the cache's */
    CHECK_HAS(r, " more couldn't be put back in their place: they're in the table lost_and_found.");
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db(), "SELECT count(*) FROM lost_and_found", -1, &st, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 3500); /* (both kept, not thrown away) */
    sqlite3_finalize(st);
    store_close();
    buf_free(&f);
}

/* Issue #87: a broken header (the first 16 bytes) - SQLite says "not a database" before reading a
   table - and a file cut short: the recovery gets the rows back. */
TEST(store_rescue_reads_past_a_broken_header_and_a_short_file) {
    const char *dir = test_tmpdir();
    Buf f = {0};
    filled(dir, 300, 2, &f);
    memcpy(f.p, "garbage garbage!", 16);
    put_file(dir, &f, f.len);
    Table t = TABLE("note", Note, note_fields), p = TABLE("person", Who, who_fields);
    store_register(&t);
    store_register(&p);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t), 300);
    CHECK_INT(tbl_count(&p), 2);
    CHECK_HAS(store_rescue_report(), "it isn't a database file any more");
    store_close();

    const char *dir2 = test_tmpdir();
    filled(dir2, 3000, 2, &f);
    put_file(dir2, &f, f.len / 4096 * 3 / 4 * 4096); /* (three quarters of it) */
    Table t2 = TABLE("note", Note, note_fields), p2 = TABLE("person", Who, who_fields);
    store_register(&t2);
    store_register(&p2);
    CHECK(store_open(dir2));
    CHECK(tbl_count(&t2) > 1000 && tbl_count(&t2) < 3000);
    CHECK_HAS(store_rescue_report(), " of 3,002 rows were kept - note: ");
    store_close();
    buf_free(&f);
}

/* Issue #87: Hemma started as a brand-new household over the real data ("Welcome to Hemma") when
   the rescue got nothing back. Now it changes nothing and won't start: the file stays as it was. */
TEST(store_refuses_a_rescue_that_gets_next_to_nothing_back) {
    const char *dir = test_tmpdir();
    char path[600], left[700];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    Buf junk = {0};
    for (int i = 0; i < 200; i++) buf_puts(&junk, "not a database at all. ");
    CHECK(file_write_atomic(path, junk.p, junk.len));
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(!store_open(dir));
    CHECK(store_refused());
    CHECK_HAS(store_refusal(), "a rescue got nothing back (0 rows). Hemma won't start on it and changed nothing");
    CHECK_INT(damaged_copy(dir), -1); /* (the file itself is untouched: no copies of it piling up) */
    Buf now = {0};
    CHECK(file_read_all(path, &now, 1 << 20));
    CHECK(now.len == junk.len && memcmp(now.p, junk.p, junk.len) == 0);
    snprintf(left, sizeof left, "%s.rescue", path);
    CHECK(!path_stat(left, NULL, NULL, NULL));
    store_close();

    /* the household's accounts gone, the notes there: still no start - nobody could sign in, and the
       first-run page would invite setting up again */
    const char *dir2 = test_tmpdir();
    Buf f = {0};
    filled(dir2, 500, 3, &f);
    size_t pg = 0;
    CHECK(page_of(&f, "person-1-of-the-house", &pg));
    memset(f.p + pg, 0xA5, 4096);
    put_file(dir2, &f, f.len);
    Table t2 = TABLE("note", Note, note_fields), p2 = TABLE("person", Who, who_fields);
    store_register(&t2);
    store_register(&p2);
    CHECK(!store_open(dir2));
    CHECK(store_refused());
    CHECK_HAS(store_refusal(), "none of the household's accounts back (500 of 503 rows)");
    store_close();
    buf_free(&f);
    buf_free(&now);
    buf_free(&junk);
}

/* Issue #88: a damaged copy per start would fill the disk (Restart=on-failure): the newest 3 stay. */
TEST(store_keeps_the_newest_three_damaged_copies) {
    const char *dir = test_tmpdir();
    Buf f = {0};
    filled(dir, 300, 0, &f);
    static const char *const old[] = {"hemma.db.damaged-2026-01-01-0100", "hemma.db.damaged-2026-01-02-0100",
                                      "hemma.db.damaged-2026-01-03-0100", "hemma.db.damaged-2026-01-04-0100"};
    char p[700];
    for (int i = 0; i < countof(old); i++) {
        snprintf(p, sizeof p, "%s/%s", dir, old[i]);
        CHECK(file_write_atomic(p, "x", 1));
    }
    snprintf(p, sizeof p, "%s/%s-wal", dir, old[0]);
    CHECK(file_write_atomic(p, "x", 1));
    size_t pg = 0;
    CHECK(page_of(&f, "note-00150-", &pg));
    memset(f.p + pg, 0xA5, 4096);
    put_file(dir, &f, f.len);
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(store_open(dir));
    store_close();
    int left = 0;
    for (int i = 0; i < countof(old); i++) {
        snprintf(p, sizeof p, "%s/%s", dir, old[i]);
        left += path_stat(p, NULL, NULL, NULL);
    }
    CHECK_INT(left, 2); /* the two oldest went, the new one and the two newest old ones stay */
    snprintf(p, sizeof p, "%s/%s-wal", dir, old[0]);
    CHECK(!path_stat(p, NULL, NULL, NULL));
    CHECK(damaged_copy(dir) > 1);
    buf_free(&f);
}

/* ======================================================= data format (issue #38) */

static long long user_version(void) {
    sqlite3_stmt *st = NULL;
    long long v = -1;
    if (sqlite3_prepare_v2(db(), "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

TEST(store_refuses_newer_data_it_would_misread) {
    /* issue #38: an older program ran on a newer file by luck - a rename it doesn't know would
       have shown it empty columns */
    const char *dir = test_tmpdir();
    Table t = TABLE("thing", Thing, thing_fields);
    store_register(&t);
    CHECK(store_open(dir));
    CHECK_INT(user_version(), store_format());
    Thing a = {0};
    tbl_put(&t, &a);
    store_close();
    /* a newer Hemma adds things: an older one still reads the file */
    static const StoreMigration adds[] = {{1, false, "base", NULL}, {2, false, "a new table", NULL}};
    store_test_migrations(adds, 2);
    Table t2 = TABLE("thing", Thing, thing_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    CHECK_INT(user_version(), 2);
    store_close();
    store_test_migrations(NULL, 0);
    Table t3 = TABLE("thing", Thing, thing_fields);
    store_register(&t3);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t3), 1);
    CHECK_INT(user_version(), 2); /* (and it stays at what the newer one made) */
    store_close();
    /* ... then changes something older ones would misread: they refuse */
    static const StoreMigration breaks[] = {
        {1, false, "base", NULL}, {2, false, "a new table", NULL}, {3, true, "a rename", NULL}};
    store_test_migrations(breaks, 3);
    store_set_program("1.2");
    Table t4 = TABLE("thing", Thing, thing_fields);
    store_register(&t4);
    CHECK(store_open(dir));
    CHECK(!store_refused());
    store_close();
    store_test_migrations(NULL, 0);
    store_set_program("1.0");
    Table t5 = TABLE("thing", Thing, thing_fields);
    store_register(&t5);
    CHECK(!store_open(dir));
    /* issue #92: said in versions people have, with the way back (the copy made before format 3) */
    CHECK(store_refused());
    char want[300];
    snprintf(want, sizeof want, "This data was last used by Hemma 1.2 (data format 3), and this Hemma 1.0 reads only up to "
             "format %d - it would misread it, so it won't start. Install Hemma 1.2 or newer again, or put back the "
             "copy made before the change: hemma.db.before-format-3.", store_format());
    CHECK_STR(store_refusal(), want);
    store_close();
    /* the newer program again: fine */
    store_test_migrations(breaks, 3);
    Table t6 = TABLE("thing", Thing, thing_fields);
    store_register(&t6);
    CHECK(store_open(dir));
    CHECK(!store_refused());
    CHECK_INT(tbl_count(&t6), 1);
    store_close();
    store_test_migrations(NULL, 0);
    store_set_program(NULL);
}

typedef struct {
    int id;
    char name[16];
} Item1;
typedef struct {
    int id;
    char title[16];
} Item2;
static const FieldDef item1_fields[] = {FIELD(Item1, id, F_INT), FIELD(Item1, name, F_STR)};
static const FieldDef item2_fields[] = {FIELD(Item2, id, F_INT), FIELD_WAS(Item2, title, F_STR, "name", 2)};
static int g_migrated;
static bool shout(void) {
    g_migrated++;
    return db_exec("UPDATE item SET title = upper(title)");
}

TEST(store_migrations_run_once_after_a_copy_and_renames_keep_data) {
    const char *dir = test_tmpdir();
    Table t = TABLE("item", Item1, item1_fields);
    store_register(&t);
    CHECK(store_open(dir));
    Item1 a = {0};
    str_copy(a.name, sizeof a.name, "box");
    tbl_put(&t, &a);
    store_close();
    /* the field is renamed (FIELD_WAS) and a migration changes the data, after a copy */
    static const StoreMigration shouted[] = {{1, false, "base", NULL}, {2, true, "titles in capitals", shout}};
    store_test_migrations(shouted, 2);
    g_migrated = 0;
    Table t2 = TABLE("item", Item2, item2_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    CHECK_INT(g_migrated, 1);
    CHECK_STR(((Item2 *)tbl_get(&t2, 1))->title, "BOX"); /* the old column's data, under its new name */
    char copy[600];
    snprintf(copy, sizeof copy, "%s/hemma.db.before-format-2", dir);
    CHECK(path_stat(copy, NULL, NULL, NULL));
    store_close();
    Table t3 = TABLE("item", Item2, item2_fields);
    store_register(&t3);
    CHECK(store_open(dir));
    CHECK_INT(g_migrated, 1); /* once */
    CHECK_STR(((Item2 *)tbl_get(&t3, 1))->title, "BOX");
    store_close();
    store_test_migrations(NULL, 0);
}

/* Issue #92: a rename needs a breaks_older migration at its format or later - without one, an older
   program re-adds the old column, empty, and shows those rows blank. */
TEST(store_names_a_rename_without_its_breaking_migration) {
    Table t = TABLE("item", Item2, item2_fields);
    store_register(&t);
    static const StoreMigration quiet[] = {{1, false, "base", NULL}, {2, false, "title for name", NULL}};
    store_test_migrations(quiet, 2);
    CHECK_STR(store_renames_unsafe(), "item.title");
    static const StoreMigration loud[] = {{1, false, "base", NULL}, {2, true, "title for name", NULL}};
    store_test_migrations(loud, 2);
    CHECK(store_renames_unsafe() == NULL);
    static const StoreMigration early[] = {{1, true, "base", NULL}, {2, false, "title for name", NULL}};
    store_test_migrations(early, 2); /* (a breaking change before the rename doesn't cover it) */
    CHECK_STR(store_renames_unsafe(), "item.title");
    store_test_migrations(NULL, 0);
    store_close();
}
