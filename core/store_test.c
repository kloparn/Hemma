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

/* The kept copy of a damaged file in dir: its size, or -1 when there's none. */
static int64_t damaged_copy(const char *dir) {
    DirIter *it = dir_open(dir);
    DirEntry e;
    int64_t size = -1;
    while (it && dir_next(it, &e))
        if (str_starts(e.name, "hemma.db.damaged-") && !str_ends(e.name, "-wal")) size = e.size;
    dir_close(it);
    return size;
}

TEST(store_rescues_what_a_damaged_database_still_holds) {
    /* issue #37: one damaged page in 3,000 notes, and Hemma started with 1,622 of them - the loader
       took the damage for the end of the table - with nothing but "malformed" in the log */
    const char *dir = test_tmpdir();
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(store_open(dir));
    db_begin();
    for (int i = 1; i <= 3000; i++) {
        Note n = {0};
        snprintf(n.text, sizeof n.text, "note-%05d-on-the-fridge", i);
        tbl_put(&t, &n);
    }
    db_commit();
    store_close(); /* (the last connection: the WAL goes into the file) */
    /* the page holding note 1500 turns to rubbish, as an SD card does after a power cut */
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    Buf f = {0};
    CHECK(file_read_all(path, &f, 64u << 20));
    size_t at = 0;
    while (at + 11 <= f.len && memcmp(f.p + at, "note-01500-", 11) != 0) at++;
    CHECK(at + 11 <= f.len);
    memset(f.p + at / 4096 * 4096, 0xA5, 4096);
    CHECK(file_write_atomic(path, f.p, f.len));

    Table t2 = TABLE("note", Note, note_fields);
    store_register(&t2);
    CHECK(store_open(dir));
    CHECK_HAS(store_rescue_report(), "the database was damaged");
    CHECK_HAS(store_rescue_report(), "rows were lost in: note");
    CHECK_HAS(store_rescue_report(), "The damaged file is kept as hemma.db.damaged-");
    int n = tbl_count(&t2);
    CHECK(n >= 2860 && n < 3000);          /* one page of notes lost (about 130 fit on one) ... */
    CHECK(tbl_get(&t2, 1500) == NULL);
    CHECK(tbl_get(&t2, 3000) != NULL);     /* ... and everything after it kept */
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

TEST(store_starts_afresh_from_a_file_that_is_no_database) {
    const char *dir = test_tmpdir();
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    Buf junk = {0};
    for (int i = 0; i < 200; i++) buf_puts(&junk, "not a database at all. ");
    CHECK(file_write_atomic(path, junk.p, junk.len));
    Table t = TABLE("note", Note, note_fields);
    store_register(&t);
    CHECK(store_open(dir));
    CHECK_HAS(store_rescue_report(), "0 rows were rescued");
    CHECK_HAS(store_rescue_report(), "rows were lost in: note");
    CHECK_INT(damaged_copy(dir), (int)junk.len);
    Note x = {0};
    tbl_put(&t, &x);
    CHECK(x.id > 0 && !store_failed());
    store_close();
    buf_free(&junk);
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
    Table t4 = TABLE("thing", Thing, thing_fields);
    store_register(&t4);
    CHECK(store_open(dir));
    store_close();
    store_test_migrations(NULL, 0);
    Table t5 = TABLE("thing", Thing, thing_fields);
    store_register(&t5);
    CHECK(!store_open(dir));
    store_close();
    /* the newer program again: fine */
    store_test_migrations(breaks, 3);
    Table t6 = TABLE("thing", Thing, thing_fields);
    store_register(&t6);
    CHECK(store_open(dir));
    CHECK_INT(tbl_count(&t6), 1);
    store_close();
    store_test_migrations(NULL, 0);
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
static const FieldDef item2_fields[] = {FIELD(Item2, id, F_INT), FIELD_WAS(Item2, title, F_STR, "name")};
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
