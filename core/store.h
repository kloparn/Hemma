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
    bool secret; /* stored, but never leaves the server (e.g. password hashes) */
} FieldDef;

#define FIELD(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), false}
#define FIELD_SECRET(T, member, type) {#member, type, offsetof(T, member), sizeof(((T *)0)->member), true}

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
bool store_open(const char *dir); /* opens dir/hemma.db, creates/extends tables, loads rows */
void store_close(void);
void store_maintain(void); /* periodic: checkpoint the WAL, let SQLite optimise */
bool store_failed(void);   /* a write failed (disk full?) since start */

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
