/* core/store.h — typed tables kept in memory, persisted in SQLite.
 *
 * A module describes its record type once:
 *
 *   typedef struct { int id; int res; int64_t start, end; char note[120]; } Booking;
 *   static const FieldDef booking_fields[] = {
 *       FIELD(Booking, id, F_INT), FIELD(Booking, res, F_INT),
 *       FIELD(Booking, start, F_I64), FIELD(Booking, end, F_I64), FIELD(Booking, note, F_STR),
 *   };
 *   Table T_booking = TABLE("booking", Booking, booking_fields);
 *   ... store_register(&T_booking) in the module's init hook.
 *
 * Every record starts with `int id`. Each table is a SQLite table of the same
 * name in data/hemma.db (one column per field; fields added later become new
 * columns automatically). All rows are loaded at start, reads are plain memory
 * access (rendering every open screen stays cheap), and tbl_put()/tbl_del()
 * write through. Ids come from AUTOINCREMENT and are never reused.
 * For data that shouldn't live in memory, or wants real queries, use db.h. */
#ifndef HEMMA_STORE_H
#define HEMMA_STORE_H

#include "base.h"
#include <stddef.h>

typedef enum { F_INT, F_I64, F_BOOL, F_STR, F_U64 } FieldType;

typedef struct {
    const char *name; /* column name: [a-z0-9_] */
    FieldType type;
    size_t offset, size;
    bool secret;     /* stored, but never leaves the server (e.g. password hashes) */
    const char *was; /* the column's old name (FIELD_WAS): renamed at the next start, data kept */
} FieldDef;

#define FIELD(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), false, NULL}
#define FIELD_SECRET(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), true, NULL}
/* A renamed field: FIELD_WAS(Note, body, F_STR, "text") moves the "text" column's data to "body".
   Older programs would read the old column (empty now): add a migration that breaks_older too. */
#define FIELD_WAS(T, member, type, old) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), false, old}

typedef struct Table {
    const char *name; /* table name: [a-z0-9_] */
    size_t rec_size;
    const FieldDef *fields; /* fields[0] must be the int id */
    int nfields;
    bool memory_only; /* never written to disk (caches, derived data) */
    bool api_hidden;  /* never exposed by /api (secrets such as session ids) */
    const char *api_owner; /* an F_INT person-id field: /api shows each member only their own rows
                              (admins see every row) - e.g. preferences, watch history */
    /* runtime */
    char *rows;
    int n, cap, next_id;
    void *impl; /* prepared statements */
} Table;

#define TABLE(name, T, fields) {name, sizeof(T), fields, (int)(sizeof(fields) / sizeof((fields)[0]))}

void store_register(Table *t); /* from module init hooks, before store_open */
/* Opens dir/hemma.db, creates/extends tables, loads rows. A damaged file (PRAGMA quick_check, or a
   table that can't be read to its end) is copied aside untouched as hemma.db.damaged-<when>, and
   every row that can still be read goes into a fresh hemma.db (store_rescue_report says what
   happened). False for a database it can't open, and for one a newer Hemma changed in a way this
   one would misread (see the data format below). */
bool store_open(const char *dir);
void store_close(void);
void store_maintain(void); /* periodic: checkpoint the WAL, let SQLite optimise */
bool store_failed(void);   /* a write failed (disk full?) since start */
bool store_damaged(void);  /* ... and SQLite said the file is damaged: a restart rescues what it can read */
/* What the last store_open did about a damaged database, for people ("" = nothing), e.g.
   "2026-10-14 10:02: the database was damaged (parts of it can't be read). The damaged file is kept
   as hemma.db.damaged-2026-10-14-1002. 2872 rows were rescued into a fresh one; rows were lost in:
   note." (SQLite's own words are in the log.) */
const char *store_rescue_report(void);

/* The data format (PRAGMA user_version): numbered migrations, oldest first. A start runs the ones
   past the file's format (a copy of the file is kept first, hemma.db.before-format-<n>, when one
   changes data); `breaks_older` = a program from before it would misread the data (a rename, a
   new meaning), so older programs refuse to start on it. Additive changes (new tables, new
   fields) need no migration at all - older programs simply don't see them. */
typedef struct {
    int format;
    bool breaks_older;
    const char *what;
    bool (*run)(void); /* SQL on db(); NULL = nothing to do */
} StoreMigration;
int store_format(void);                                    /* the newest format this program knows */
void store_test_migrations(const StoreMigration *m, int n); /* tests: another list (NULL = the real one) */

static inline int tbl_count(const Table *t) { return t->n; }
static inline void *tbl_at(const Table *t, int i) { return t->rows + (size_t)i * t->rec_size; }
void *tbl_get(const Table *t, int id); /* NULL if missing */
/* Upsert a copy of rec. id 0 = new: the id is assigned and written back into rec.
 * Returns the stored row — valid until the next put/del on this table. */
void *tbl_put(Table *t, void *rec);
bool tbl_del(Table *t, int id);
/* Update the in-memory row only (frequent, unimportant changes such as
 * "last seen"); a later tbl_put persists it. */
void *tbl_put_quiet(Table *t, void *rec);

void tbl_to_json(const Table *t, const void *rec, Buf *out);
Table *store_table(const char *name);
int store_tables(Table ***out);

#endif
