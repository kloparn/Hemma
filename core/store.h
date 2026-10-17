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
    int was_format;  /* ... the data format that renamed it (it needs a breaks_older migration from there) */
} FieldDef;

#define FIELD(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), false, NULL}
#define FIELD_SECRET(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), true, NULL}
/* A renamed field: FIELD_WAS(Note, body, F_STR, "text", 2) moves the "text" column's data to "body" in data
   format 2. Older programs would read the old column (empty now): format 2 (or later) must be a migration
   that breaks_older - store_renames_unsafe() checks it, and store_test.c fails without one (issue #92). */
#define FIELD_WAS(T, member, type, old, format)                                                                       \
    {#member, type, offsetof(T, member), sizeof(((T *)0)->member), false, old, format}

typedef struct Table {
    const char *name; /* table name: [a-z0-9_] */
    size_t rec_size;
    const FieldDef *fields; /* fields[0] must be the int id */
    int nfields;
    bool memory_only; /* never written to disk (caches, derived data) */
    bool api_hidden;  /* never exposed by /api (secrets such as session ids) */
    const char *api_owner; /* an F_INT person-id field: /api shows each member only their own rows
                              (admins see every row) - e.g. preferences, watch history */
    const char *what; /* what its rows are, in the household's words ("notes on the board"): a rescue says
                         what it kept in them. Set it in the module's init; NULL = the table's name */
    /* runtime */
    char *rows;
    int n, cap, next_id;
    void *impl; /* prepared statements */
} Table;

#define TABLE(name, T, fields) {name, sizeof(T), fields, (int)(sizeof(fields) / sizeof((fields)[0]))}

void store_register(Table *t); /* from module init hooks, before store_open */
/* Opens dir/hemma.db, creates/extends tables, loads rows. A damaged file (PRAGMA quick_check, or a
   table that can't be read to its end) is copied aside untouched as hemma.db.damaged-<when> (the
   newest 3 stay), and SQLite's recovery (third_party/sqlite/recover.c) reads every row it still can
   into a fresh hemma.db (store_rescue_report says what was kept). When that gets next to nothing
   back - no rows, none of the accounts, under a tenth of the census - nothing is changed and it
   refuses (issue #87: Hemma started as a brand-new household). After every healthy open it notes
   each table's rows beside the database (hemma.db.counts), what a rescue compares against.
   False for a database it can't open, a refused rescue, and data a newer Hemma changed in a way
   this one would misread (see the data format below): store_refused() tells those last two apart. */
bool store_open(const char *dir);
void store_close(void);
void store_maintain(void); /* periodic: checkpoint the WAL, let SQLite optimise, note the rows */
bool store_failed(void);   /* a write failed (disk full?) since start */
bool store_damaged(void);  /* ... and SQLite said the file is damaged: a restart rescues what it can read */
/* What the last store_open did about a damaged database, for people ("" = nothing), e.g.
   "2026-10-14 10:02: Hemma's data was damaged (parts of it couldn't be read). Nothing was deleted: the
   damaged file is kept as hemma.db.damaged-2026-10-14-1002. 2,944 of 3,000 rows were kept - notes on
   the board: 2,944 of 3,000." (SQLite's own words are in the log.) */
const char *store_rescue_report(void);
/* store_open left the data alone on purpose - a rescue that got next to nothing back, or data from a
   newer Hemma - and store_refusal() says why, for people (it ends up in the log file too); restarting
   won't change it (the program exits with its own code, HEMMA_EXIT_DATA). */
bool store_refused(void);
const char *store_refusal(void);
void store_set_program(const char *version); /* this Hemma's version: kept with the data, named in refusals */
const char *store_renames_unsafe(void);      /* a FIELD_WAS without its breaks_older migration ("table.field") */

/* ---- backups (issue #35) ----
   store_backup: a consistent copy of the open database at `to` (VACUUM INTO over its own read-only
   connection, so ANY thread may call it while the household carries on), synced to the disk and
   checked; written as `to`.part first, so a failed one leaves nothing. rows = rows of Hemma's
   tables in it. store_check_copy: may this file be put back? (PRAGMA quick_check, Hemma's tables,
   not a newer Hemma's data format; no tables need to be registered.) */
bool store_backup(const char *to, long long *rows, char *why, size_t cap);
bool store_check_copy(const char *path, long long *rows, char *why, size_t cap);
/* A backup to go back to - a .db, or a download's .tar (the database + hemma-secret.txt) - checked and
   placed as <data>/hemma.db.restore; the next store_open puts it in place, keeping what was there
   as hemma.db.before-restore-<day>-<hhmm> (the newest 3 stay). label = what people call it. A
   .tar's HEMMA_AUTH_SALT comes back in secret ("" = none in it). Any thread; no open store needed. */
bool store_stage_restore(const char *data_dir, const char *file, const char *label, char *secret, size_t secret_cap,
                         char *why, size_t cap);
bool store_restore_waiting(const char *data_dir, char *label, size_t cap); /* one is staged (its label) */
void store_unstage_restore(const char *data_dir);                           /* ... not any more */
/* What the last store_open did about a staged backup, for people ("" = nothing), e.g. "2026-10-17
   09:30: the backup hemma-2026-10-16-0330.db was put back (812 rows). What was there before is kept
   as hemma.db.before-restore-2026-10-17-0930." */
const char *store_restore_report(void);

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
