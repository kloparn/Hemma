#include "../tests/test.h"
#include "db.h"
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
