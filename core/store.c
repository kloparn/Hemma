/* core/store.c — typed in-memory tables persisted in SQLite (see store.h),
 * plus the shared connection for direct SQL (see db.h). */
#include "store.h"
#include "db.h"
#include "json.h"
#include "plat.h"

#include <limits.h>

#define MAX_TABLES 64

typedef struct {
    sqlite3_stmt *insert, *upsert, *del;
} Stmts;

static Table *tables[MAX_TABLES];
static int ntables;
static sqlite3 *g_db;
static bool g_failed;
static int g_tx_depth;
/* Prepared statements by SQL text. Never evicted: the set of distinct SQL
   strings is fixed by the code, and evicting one an outer loop is still
   stepping would be a use-after-free. */
static struct PrepEntry {
    char *sql;
    sqlite3_stmt *st;
} *g_prep;
static int g_nprep, g_capprep;

/* =============================================================== registry */

static bool valid_ident(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}

void store_register(Table *t) {
    for (int i = 0; i < ntables; i++)
        if (tables[i] == t || str_eq(tables[i]->name, t->name)) return;
    bool ok = valid_ident(t->name) && t->nfields >= 1 && str_eq(t->fields[0].name, "id") && t->fields[0].type == F_INT &&
              t->fields[0].offset == 0;
    for (int i = 0; ok && i < t->nfields; i++) ok = valid_ident(t->fields[i].name);
    if (!ok) {
        log_error("store: table \"%s\" has a bad definition (lower-case names, fields[0] = int id)", t->name);
        return;
    }
    if (ntables == MAX_TABLES) {
        log_error("store: too many tables (max %d)", MAX_TABLES);
        return;
    }
    if (t->next_id < 1) t->next_id = 1;
    tables[ntables++] = t;
}

Table *store_table(const char *name) {
    for (int i = 0; i < ntables; i++)
        if (str_eq(tables[i]->name, name)) return tables[i];
    return NULL;
}

int store_tables(Table ***out) {
    *out = tables;
    return ntables;
}

bool store_failed(void) { return g_failed; }

/* ============================================================ memory rows */

static int rec_id(const void *rec) {
    int id;
    memcpy(&id, rec, sizeof id);
    return id;
}

static int lower_bound(const Table *t, int id) {
    int lo = 0, hi = t->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (rec_id(tbl_at(t, mid)) < id) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void *tbl_get(const Table *t, int id) {
    int i = lower_bound(t, id);
    return (i < t->n && rec_id(tbl_at(t, i)) == id) ? tbl_at(t, i) : NULL;
}

static void terminate_strings(const Table *t, void *rec) {
    for (int i = 0; i < t->nfields; i++)
        if (t->fields[i].type == F_STR) ((char *)rec)[t->fields[i].offset + t->fields[i].size - 1] = '\0';
}

static void *mem_upsert(Table *t, void *rec) {
    int id = rec_id(rec);
    if (id <= 0) {
        id = t->next_id;
        memcpy(rec, &id, sizeof id);
    }
    if (id >= t->next_id) t->next_id = id + 1;
    int i = lower_bound(t, id);
    if (i < t->n && rec_id(tbl_at(t, i)) == id) {
        memmove(tbl_at(t, i), rec, t->rec_size);
        return tbl_at(t, i);
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->rows = xrealloc(t->rows, (size_t)t->cap * t->rec_size);
    }
    memmove(tbl_at(t, i + 1), tbl_at(t, i), (size_t)(t->n - i) * t->rec_size);
    memcpy(tbl_at(t, i), rec, t->rec_size);
    t->n++;
    return tbl_at(t, i);
}

static bool mem_remove(Table *t, int id) {
    int i = lower_bound(t, id);
    if (i >= t->n || rec_id(tbl_at(t, i)) != id) return false;
    memmove(tbl_at(t, i), tbl_at(t, i + 1), (size_t)(t->n - i - 1) * t->rec_size);
    t->n--;
    return true;
}

/* ===================================================================== SQL */

sqlite3 *db(void) { return g_db; }

bool db_exec(const char *sql) {
    if (!g_db) return false;
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        log_error("sql: %s  [%.120s]", err ? err : "?", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

sqlite3_stmt *db_prep(const char *sql) {
    if (!g_db) return NULL;
    for (int i = 0; i < g_nprep; i++) {
        if (strcmp(g_prep[i].sql, sql) == 0) {
            sqlite3_reset(g_prep[i].st);
            sqlite3_clear_bindings(g_prep[i].st);
            return g_prep[i].st;
        }
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v3(g_db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, NULL) != SQLITE_OK) {
        log_error("sql: %s  [%.120s]", sqlite3_errmsg(g_db), sql);
        return NULL;
    }
    if (g_nprep == g_capprep) {
        g_capprep = g_capprep ? g_capprep * 2 : 64;
        g_prep = xrealloc(g_prep, (size_t)g_capprep * sizeof *g_prep);
    }
    int slot = g_nprep++;
    g_prep[slot].sql = xstrdup(sql);
    g_prep[slot].st = st;
    return st;
}

/* Reset every cached statement so no read transaction lingers (lets the WAL checkpoint). */
static void reset_cached(void) {
    for (int i = 0; i < g_nprep; i++) sqlite3_reset(g_prep[i].st);
}

void db_idle(void) {
    if (g_tx_depth == 0) reset_cached();
}

void db_begin(void) {
    if (g_tx_depth++ == 0) db_exec("BEGIN");
}

void db_commit(void) {
    if (g_tx_depth <= 0) return;
    if (--g_tx_depth == 0) {
        reset_cached();
        if (!db_exec("COMMIT")) {
            /* disk full / I/O error usually shows up here: the change is in memory
               only. Make that visible (store_failed) and leave no transaction open. */
            g_failed = true;
            if (!sqlite3_get_autocommit(g_db)) db_exec("ROLLBACK");
        }
    }
}

/* ============================================================ table SQL */

static void column_def(Buf *b, const FieldDef *f) {
    if (f->type == F_STR || f->type == F_U64) buf_printf(b, "\"%s\" TEXT NOT NULL DEFAULT ''", f->name);
    else buf_printf(b, "\"%s\" INTEGER NOT NULL DEFAULT 0", f->name);
}

static bool has_column(const Table *t, const char *col) {
    Buf q = {0};
    buf_printf(&q, "SELECT 1 FROM pragma_table_info('%s') WHERE name = ?", t->name);
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(g_db, buf_cstr(&q), -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, col, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    buf_free(&q);
    return found;
}

static bool ensure_schema(const Table *t) {
    Buf sql = {0};
    buf_printf(&sql, "CREATE TABLE IF NOT EXISTS \"%s\" (\"id\" INTEGER PRIMARY KEY AUTOINCREMENT", t->name);
    for (int i = 1; i < t->nfields; i++) {
        buf_puts(&sql, ", ");
        column_def(&sql, &t->fields[i]);
    }
    buf_puts(&sql, ")");
    bool ok = db_exec(buf_cstr(&sql));
    for (int i = 1; ok && i < t->nfields; i++) {
        if (has_column(t, t->fields[i].name)) continue;
        buf_reset(&sql);
        buf_printf(&sql, "ALTER TABLE \"%s\" ADD COLUMN ", t->name);
        column_def(&sql, &t->fields[i]);
        ok = db_exec(buf_cstr(&sql));
        if (ok) log_info("store: added column %s.%s", t->name, t->fields[i].name);
    }
    buf_free(&sql);
    return ok;
}

static void read_column(const FieldDef *f, sqlite3_stmt *st, int col, char *rec) {
    switch (f->type) {
    case F_INT: {
        sqlite3_int64 v = sqlite3_column_int64(st, col);
        int x = v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
        memcpy(rec + f->offset, &x, sizeof x);
        break;
    }
    case F_I64: {
        int64_t v = sqlite3_column_int64(st, col);
        memcpy(rec + f->offset, &v, sizeof v);
        break;
    }
    case F_BOOL: rec[f->offset] = sqlite3_column_int(st, col) != 0; break;
    case F_STR: {
        const char *s = (const char *)sqlite3_column_text(st, col);
        str_copy(rec + f->offset, f->size, s ? s : "");
        break;
    }
    case F_U64: {
        const char *s = (const char *)sqlite3_column_text(st, col);
        uint64_t v = s ? strtoull(s, NULL, 16) : 0;
        memcpy(rec + f->offset, &v, sizeof v);
        break;
    }
    }
}

static void bind_fields(sqlite3_stmt *st, const Table *t, const char *rec, int first) {
    for (int i = 1; i < t->nfields; i++) {
        const FieldDef *f = &t->fields[i];
        int p = first + i - 1;
        switch (f->type) {
        case F_INT: {
            int v;
            memcpy(&v, rec + f->offset, sizeof v);
            sqlite3_bind_int(st, p, v);
            break;
        }
        case F_I64: {
            int64_t v;
            memcpy(&v, rec + f->offset, sizeof v);
            sqlite3_bind_int64(st, p, v);
            break;
        }
        case F_BOOL: sqlite3_bind_int(st, p, rec[f->offset] ? 1 : 0); break;
        case F_STR:
            sqlite3_bind_text(st, p, rec + f->offset, (int)strnlen(rec + f->offset, f->size), SQLITE_TRANSIENT);
            break;
        case F_U64: {
            uint64_t v;
            memcpy(&v, rec + f->offset, sizeof v);
            char hex[17];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)v);
            sqlite3_bind_text(st, p, hex, 16, SQLITE_TRANSIENT);
            break;
        }
        }
    }
}

static bool prepare_stmts(Table *t) {
    Stmts *s = xcalloc(1, sizeof *s);
    Buf a = {0}, b = {0}, c = {0};
    if (t->nfields == 1) {
        buf_printf(&a, "INSERT INTO \"%s\" DEFAULT VALUES", t->name);
        buf_printf(&b, "INSERT OR IGNORE INTO \"%s\" (\"id\") VALUES (?)", t->name);
    } else {
        buf_printf(&a, "INSERT INTO \"%s\" (", t->name);
        buf_printf(&b, "INSERT INTO \"%s\" (\"id\"", t->name);
        for (int i = 1; i < t->nfields; i++) {
            buf_printf(&a, "%s\"%s\"", i > 1 ? "," : "", t->fields[i].name);
            buf_printf(&b, ",\"%s\"", t->fields[i].name);
        }
        buf_puts(&a, ") VALUES (");
        buf_puts(&b, ") VALUES (?");
        for (int i = 1; i < t->nfields; i++) {
            buf_puts(&a, i > 1 ? ",?" : "?");
            buf_puts(&b, ",?");
        }
        buf_puts(&a, ")");
        buf_puts(&b, ") ON CONFLICT(\"id\") DO UPDATE SET ");
        for (int i = 1; i < t->nfields; i++)
            buf_printf(&b, "%s\"%s\"=excluded.\"%s\"", i > 1 ? "," : "", t->fields[i].name, t->fields[i].name);
    }
    buf_printf(&c, "DELETE FROM \"%s\" WHERE \"id\" = ?", t->name);
    bool ok = sqlite3_prepare_v3(g_db, buf_cstr(&a), -1, SQLITE_PREPARE_PERSISTENT, &s->insert, NULL) == SQLITE_OK &&
              sqlite3_prepare_v3(g_db, buf_cstr(&b), -1, SQLITE_PREPARE_PERSISTENT, &s->upsert, NULL) == SQLITE_OK &&
              sqlite3_prepare_v3(g_db, buf_cstr(&c), -1, SQLITE_PREPARE_PERSISTENT, &s->del, NULL) == SQLITE_OK;
    if (!ok) log_error("store: preparing %s: %s", t->name, sqlite3_errmsg(g_db));
    buf_free(&a);
    buf_free(&b);
    buf_free(&c);
    t->impl = s;
    return ok;
}

static void load(Table *t) {
    Buf sql = {0};
    buf_puts(&sql, "SELECT \"id\"");
    for (int i = 1; i < t->nfields; i++) buf_printf(&sql, ",\"%s\"", t->fields[i].name);
    buf_printf(&sql, " FROM \"%s\" ORDER BY \"id\"", t->name);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, buf_cstr(&sql), -1, &st, NULL) == SQLITE_OK) {
        char *rec = xmalloc(t->rec_size);
        while (sqlite3_step(st) == SQLITE_ROW) {
            memset(rec, 0, t->rec_size);
            int id = sqlite3_column_int(st, 0);
            memcpy(rec, &id, sizeof id);
            for (int i = 1; i < t->nfields; i++) read_column(&t->fields[i], st, i, rec);
            mem_upsert(t, rec);
        }
        free(rec);
    }
    sqlite3_finalize(st);
    buf_free(&sql);
    /* AUTOINCREMENT remembers the highest id ever used: never hand a deleted
       row's id out again (old bookings would attach to a new person) */
    sqlite3_stmt *seq = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT seq FROM sqlite_sequence WHERE name = ?", -1, &seq, NULL) == SQLITE_OK) {
        sqlite3_bind_text(seq, 1, t->name, -1, SQLITE_STATIC);
        if (sqlite3_step(seq) == SQLITE_ROW) {
            long long s = sqlite3_column_int64(seq, 0);
            if (s >= t->next_id && s < INT_MAX) t->next_id = (int)s + 1;
        }
    }
    sqlite3_finalize(seq);
}

/* =========================================================== open / close */

bool store_open(const char *dir) {
    if (!dir_make(dir)) {
        log_error("cannot create data directory %s", dir);
        return false;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        log_error("cannot open %s: %s", path, g_db ? sqlite3_errmsg(g_db) : "out of memory");
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }
    sqlite3_busy_timeout(g_db, 3000);
    db_exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    bool ok = true;
    long long rows = 0;
    db_begin();
    for (int i = 0; i < ntables && ok; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        ok = ensure_schema(t) && prepare_stmts(t);
        if (ok) load(t);
        rows += t->n;
    }
    db_commit();
    if (ok) log_info("store: %lld rows from %s (SQLite %s)", rows, path, sqlite3_libversion());
    return ok;
}

void store_maintain(void) {
    reset_cached();
    db_exec("PRAGMA optimize; PRAGMA wal_checkpoint(TRUNCATE);");
}

void store_close(void) {
    for (int i = 0; i < ntables; i++) {
        Table *t = tables[i];
        if (t->impl) {
            Stmts *s = t->impl;
            sqlite3_finalize(s->insert);
            sqlite3_finalize(s->upsert);
            sqlite3_finalize(s->del);
            free(s);
            t->impl = NULL;
        }
        free(t->rows);
        t->rows = NULL;
        t->n = t->cap = 0;
        t->next_id = 1;
    }
    ntables = 0;
    for (int i = 0; i < g_nprep; i++) {
        sqlite3_finalize(g_prep[i].st);
        free(g_prep[i].sql);
        g_prep[i].st = NULL;
        g_prep[i].sql = NULL;
    }
    g_nprep = 0;
    g_tx_depth = 0;
    if (g_db) sqlite3_close_v2(g_db);
    g_db = NULL;
}

/* ================================================================ writes */

static void write_failed(const Table *t) {
    g_failed = true;
    log_error("store: writing %s failed: %s", t->name, g_db ? sqlite3_errmsg(g_db) : "no database");
}

void *tbl_put(Table *t, void *rec) {
    terminate_strings(t, rec);
    Stmts *s = t->impl;
    if (t->memory_only || !s) return mem_upsert(t, rec);
    int id = rec_id(rec);
    sqlite3_stmt *st = id > 0 ? s->upsert : s->insert;
    sqlite3_reset(st);
    if (id > 0) sqlite3_bind_int(st, 1, id);
    bind_fields(st, t, rec, id > 0 ? 2 : 1);
    if (sqlite3_step(st) == SQLITE_DONE) {
        if (id <= 0) {
            id = (int)sqlite3_last_insert_rowid(g_db);
            memcpy(rec, &id, sizeof id);
        }
    } else {
        write_failed(t);
    }
    sqlite3_reset(st);
    return mem_upsert(t, rec);
}

void *tbl_put_quiet(Table *t, void *rec) {
    terminate_strings(t, rec);
    return mem_upsert(t, rec);
}

bool tbl_del(Table *t, int id) {
    if (!mem_remove(t, id)) return false;
    Stmts *s = t->impl;
    if (!t->memory_only && s) {
        sqlite3_reset(s->del);
        sqlite3_bind_int(s->del, 1, id);
        if (sqlite3_step(s->del) != SQLITE_DONE) write_failed(t);
        sqlite3_reset(s->del);
    }
    return true;
}

/* ================================================================== JSON */

void tbl_to_json(const Table *t, const void *rec, Buf *out) {
    const char *r = rec;
    buf_putc(out, '{');
    for (int i = 0, written = 0; i < t->nfields; i++) {
        const FieldDef *f = &t->fields[i];
        if (f->secret) continue;
        if (written++) buf_putc(out, ',');
        json_str(out, f->name);
        buf_putc(out, ':');
        switch (f->type) {
        case F_INT: {
            int v;
            memcpy(&v, r + f->offset, sizeof v);
            buf_printf(out, "%d", v);
            break;
        }
        case F_I64: {
            int64_t v;
            memcpy(&v, r + f->offset, sizeof v);
            buf_printf(out, "%lld", (long long)v);
            break;
        }
        case F_BOOL: buf_puts(out, r[f->offset] ? "true" : "false"); break;
        case F_STR: json_strn(out, r + f->offset, strnlen(r + f->offset, f->size)); break;
        case F_U64: {
            uint64_t v;
            memcpy(&v, r + f->offset, sizeof v);
            buf_printf(out, "\"%016llx\"", (unsigned long long)v);
            break;
        }
        }
    }
    buf_putc(out, '}');
}
