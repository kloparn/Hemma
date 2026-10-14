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
static int g_last_err;     /* SQLite's code for the last write that failed */
static char g_rescue[600]; /* store_rescue_report() */
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
    for (int i = 0; ok && i < t->nfields; i++) ok = valid_ident(t->fields[i].name) && (!t->fields[i].was || valid_ident(t->fields[i].was));
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

/* SQLite's word for a damaged file (corrupt pages, or not a database at all) */
static bool damage(int rc) {
    rc &= 0xff;
    return rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB;
}

bool store_damaged(void) { return g_failed && damage(g_last_err); }
const char *store_rescue_report(void) { return g_rescue; }

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
            g_last_err = sqlite3_extended_errcode(g_db);
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
        const FieldDef *f = &t->fields[i];
        if (has_column(t, f->name)) continue;
        buf_reset(&sql);
        if (f->was && has_column(t, f->was)) { /* FIELD_WAS: the data moves with the name */
            buf_printf(&sql, "ALTER TABLE \"%s\" RENAME COLUMN \"%s\" TO \"%s\"", t->name, f->was, f->name);
            ok = db_exec(buf_cstr(&sql));
            if (ok) log_info("store: renamed column %s.%s to %s", t->name, f->was, f->name);
            continue;
        }
        buf_printf(&sql, "ALTER TABLE \"%s\" ADD COLUMN ", t->name);
        column_def(&sql, f);
        ok = db_exec(buf_cstr(&sql));
        if (ok) log_info("store: added column %s.%s", t->name, f->name);
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

/* 1 = every row read, 0 = the file is damaged (issue #37: the loop used to stop at a damaged page
   as if the table ended there - Hemma ran with half the notes and said nothing), -1 = another
   failure. */
static int load(Table *t) {
    Buf sql = {0};
    buf_puts(&sql, "SELECT \"id\"");
    for (int i = 1; i < t->nfields; i++) buf_printf(&sql, ",\"%s\"", t->fields[i].name);
    buf_printf(&sql, " FROM \"%s\" ORDER BY \"id\"", t->name);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g_db, buf_cstr(&sql), -1, &st, NULL);
    if (rc == SQLITE_OK) {
        char *rec = xmalloc(t->rec_size);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            memset(rec, 0, t->rec_size);
            int id = sqlite3_column_int(st, 0);
            memcpy(rec, &id, sizeof id);
            for (int i = 1; i < t->nfields; i++) read_column(&t->fields[i], st, i, rec);
            mem_upsert(t, rec);
        }
        free(rec);
    }
    int result = rc == SQLITE_DONE ? 1 : damage(rc) ? 0 : -1;
    if (result <= 0) log_error("store: reading %s stopped after %d rows: %s", t->name, t->n, sqlite3_errmsg(g_db));
    sqlite3_finalize(st);
    buf_free(&sql);
    if (result <= 0) return result;
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
    return 1;
}

/* ============================================================ data format */

/* Oldest first; the last one's number is this program's format. Issue #38: before these the file
   had no version, so a rename emptied a column and an older program ran on newer data by luck. */
static const StoreMigration MIGRATIONS[] = {
    {1, false, "the first numbered data format (nothing changes)", NULL},
};
static const StoreMigration *g_migs = MIGRATIONS;
static int g_nmigs = (int)countof(MIGRATIONS);

int store_format(void) { return g_nmigs ? g_migs[g_nmigs - 1].format : 0; }

void store_test_migrations(const StoreMigration *m, int n) {
    g_migs = m ? m : MIGRATIONS;
    g_nmigs = m ? n : (int)countof(MIGRATIONS);
}

static long long sql_int(sqlite3 *d, const char *sql) { /* the first column of the first row, 0 if none */
    sqlite3_stmt *st = NULL;
    long long v = 0;
    if (sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* store_meta: what PRAGMA user_version can't hold - "min_reader", the oldest format that still
   reads this data right (raised by migrations that break_older). */
static bool meta_set(const char *k, long long v) {
    sqlite3_stmt *st = NULL;
    bool ok = db_exec("CREATE TABLE IF NOT EXISTS store_meta (k TEXT PRIMARY KEY, v INTEGER NOT NULL)") &&
              sqlite3_prepare_v2(g_db, "INSERT INTO store_meta (k, v) VALUES (?1, ?2) ON CONFLICT(k) DO UPDATE SET v = ?2",
                                 -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, v);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    return ok;
}

/* Before anything is changed: false = a newer Hemma's data this one would misread. */
static bool format_ok(const char *path, int *have, bool *risky) {
    *have = (int)sql_int(g_db, "PRAGMA user_version");
    int mine = store_format(), floor = (int)sql_int(g_db, "SELECT v FROM store_meta WHERE k = 'min_reader'");
    if (*have > mine && floor > mine) {
        log_error("store: %s was changed by a newer Hemma (data format %d, read right only from format %d on); this "
                  "Hemma knows format %d and would misread it. Put the newer program back, or restore a backup "
                  "made before it.",
                  path, *have, floor, mine);
        return false;
    }
    if (*have > mine)
        log_info("store: %s has data format %d (this Hemma knows %d): nothing it would misread changed", path, *have, mine);
    *risky = false;
    for (int i = 0; i < g_nmigs; i++)
        *risky |= g_migs[i].format > *have && (g_migs[i].run || g_migs[i].breaks_older);
    return true;
}

/* A copy before a migration changes data: hemma.db.before-format-<n> (VACUUM INTO: consistent). */
static void snapshot(const char *path) {
    char to[700];
    snprintf(to, sizeof to, "%s.before-format-%d", path, store_format());
    file_remove(to);
    char *sql = sqlite3_mprintf("VACUUM INTO %Q", to);
    if (sql && db_exec(sql)) log_info("store: kept a copy as %s before changing the data format", to);
    sqlite3_free(sql);
}

static bool migrate(int have) {
    int floor = (int)sql_int(g_db, "SELECT v FROM store_meta WHERE k = 'min_reader'");
    for (int i = 0; i < g_nmigs; i++) {
        const StoreMigration *m = &g_migs[i];
        if (m->format <= have) continue;
        if (m->run && !m->run()) {
            log_error("store: changing the data to format %d (%s) failed - nothing was changed", m->format, m->what);
            return false;
        }
        if (m->breaks_older) floor = m->format;
        log_info("store: data format %d: %s", m->format, m->what);
    }
    char sql[48];
    snprintf(sql, sizeof sql, "PRAGMA user_version = %d", store_format());
    return meta_set("min_reader", floor) && db_exec(sql);
}

/* ====================================================== a damaged database */

/* PRAGMA quick_check: 1 = damaged (why says how), 0 = fine, -1 = unreadable for another reason
   (locked, an I/O error) - that's no reason to rebuild it. */
static int check_file(char *why, size_t cap) {
    why[0] = '\0';
    sqlite3_stmt *st = NULL;
    int verdict = 0, rc = sqlite3_prepare_v2(g_db, "PRAGMA quick_check(3)", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(st, 0);
            if (s && strcmp(s, "ok") != 0) {
                verdict = 1;
                if (!why[0]) str_copy(why, cap, s);
            }
        }
    }
    if (rc != SQLITE_DONE) {
        if (!why[0]) str_copy(why, cap, sqlite3_errmsg(g_db));
        if (!verdict) verdict = damage(rc) ? 1 : -1;
    }
    sqlite3_finalize(st);
    return verdict;
}

static bool copy_file(const char *from, const char *to) {
    if (!path_stat(from, NULL, NULL, NULL)) return true; /* (nothing there: no WAL, say) */
    FILE *in = file_open(from, "rb"), *out = in ? file_open(to, "wb") : NULL;
    bool ok = in && out;
    char buf[32 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in) && file_sync(out);
    if (out && fclose(out) != 0) ok = false;
    if (in) fclose(in);
    return ok;
}

/* Does the damaged file's table have that column? 1 yes, 0 no, -1 it can't tell (damage). */
static int src_has(sqlite3 *src, const char *table, const char *col) {
    sqlite3_stmt *st = NULL;
    int r = -1;
    if (sqlite3_prepare_v2(src, "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, col, -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        r = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    return r;
}

static void unprepare(Table *t) {
    Stmts *s = t->impl;
    if (!s) return;
    sqlite3_finalize(s->insert);
    sqlite3_finalize(s->upsert);
    sqlite3_finalize(s->del);
    free(s);
    t->impl = NULL;
}

/* The first row after id `after` (st: "... WHERE id > ?1 ORDER BY id LIMIT 1"): ROW, DONE or an error. */
static int probe(sqlite3_stmt *st, long long after) {
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, after);
    return sqlite3_step(st);
}

/* Reading on from *from failed (a damaged page): the first point past the damage that reads again
   - 1, 2, 4, ... ids on, then back by halves - so only the damaged page's rows are lost. False:
   nothing after it reads. */
static bool skip_damage(sqlite3_stmt *st, long long *from) {
    long long bad = *from, good = -1;
    for (long long step = 1; bad + step <= INT_MAX; step *= 2) {
        int rc = probe(st, bad + step);
        if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
            good = bad + step;
            break;
        }
        bad += step;
    }
    if (good < 0) return false;
    while (good - bad > 1) {
        long long mid = bad + (good - bad) / 2;
        int rc = probe(st, mid);
        if (rc == SQLITE_ROW || rc == SQLITE_DONE) good = mid;
        else bad = mid;
    }
    *from = good;
    return true;
}

/* Every row of t the damaged file still gives, into the fresh database (g_db), in id order. A
   read that fails - a damaged page - skips past it (skip_damage): the rows on the damaged page are
   lost, the ones after it aren't. */
static void rescue_table(sqlite3 *src, Table *t, long long *rows, bool *lost) {
    int present = src_has(src, t->name, "id");
    if (present <= 0) {
        *lost |= present < 0; /* (a table that didn't exist yet loses nothing) */
        return;
    }
    Buf sql = {0};
    buf_puts(&sql, "SELECT \"id\"");
    for (int i = 1; i < t->nfields; i++) {
        const FieldDef *f = &t->fields[i];
        if (src_has(src, t->name, f->name) > 0) buf_printf(&sql, ",\"%s\"", f->name);
        else if (f->was && src_has(src, t->name, f->was) > 0) buf_printf(&sql, ",\"%s\"", f->was);
        else buf_puts(&sql, f->type == F_STR || f->type == F_U64 ? ",''" : ",0");
    }
    buf_printf(&sql, " FROM \"%s\" WHERE \"id\" > ?1 ORDER BY \"id\" LIMIT 1", t->name);
    sqlite3_stmt *st = NULL;
    Stmts *s = t->impl;
    if (sqlite3_prepare_v2(src, buf_cstr(&sql), -1, &st, NULL) != SQLITE_OK || !s) {
        *lost = true;
        sqlite3_finalize(st);
        buf_free(&sql);
        return;
    }
    char *rec = xmalloc(t->rec_size);
    long long last = 0;
    for (;;) {
        int rc = probe(st, last);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) { /* a damaged stretch: over it */
            *lost = true;
            if (!skip_damage(st, &last)) break;
            continue;
        }
        long long id = sqlite3_column_int64(st, 0);
        if (id <= last || id > INT_MAX) break; /* (a damaged index can't send it round in circles) */
        memset(rec, 0, t->rec_size);
        int iid = (int)id;
        memcpy(rec, &iid, sizeof iid);
        for (int i = 1; i < t->nfields; i++) read_column(&t->fields[i], st, i, rec);
        terminate_strings(t, rec);
        sqlite3_reset(s->upsert);
        sqlite3_bind_int(s->upsert, 1, iid);
        bind_fields(s->upsert, t, rec, 2);
        if (sqlite3_step(s->upsert) == SQLITE_DONE) (*rows)++;
        sqlite3_reset(s->upsert);
        last = id;
    }
    free(rec);
    sqlite3_finalize(st);
    buf_free(&sql);
    /* ids are never handed out twice: the old sequence, or - unreadable, with rows lost - room above */
    sqlite3_stmt *q = NULL;
    long long seq = -1;
    if (sqlite3_prepare_v2(src, "SELECT seq FROM sqlite_sequence WHERE name = ?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, t->name, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) seq = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    if (seq < 0 && *lost) seq = last + 1000;
    if (seq > last && seq < INT_MAX) {
        char up[200];
        snprintf(up, sizeof up, "DELETE FROM sqlite_sequence WHERE name = '%s'; INSERT INTO sqlite_sequence (name, seq) "
                 "VALUES ('%s', %lld);", t->name, t->name, seq); /* (table names are [a-z0-9_]) */
        db_exec(up);
    }
}

/* The damaged file is copied aside untouched (hemma.db.damaged-<when>, its WAL too); every row
   that can still be read goes into a fresh file that takes its place. g_db is the damaged one. */
static bool rescue(const char *path, const char *why) {
    char day[11], hm[6], kept[700], tmp[700], side[720];
    int64_t now = now_sec();
    fmt_ymd(now, day);
    fmt_hm(now, hm);
    snprintf(kept, sizeof kept, "%s.damaged-%s-%c%c%c%c", path, day, hm[0], hm[1], hm[3], hm[4]);
    for (int k = 2; k < 100 && path_stat(kept, NULL, NULL, NULL); k++)
        snprintf(kept, sizeof kept, "%s.damaged-%s-%c%c%c%c-%d", path, day, hm[0], hm[1], hm[3], hm[4], k);
    snprintf(side, sizeof side, "%s-wal", path);
    char kept_wal[720];
    snprintf(kept_wal, sizeof kept_wal, "%s-wal", kept);
    if (!copy_file(path, kept) || !copy_file(side, kept_wal)) {
        log_error("store: couldn't keep a copy of the damaged %s as %s - not touching it (is the disk full?)", path, kept);
        return false;
    }
    snprintf(tmp, sizeof tmp, "%s.rescue", path);
    file_remove(tmp);
    snprintf(side, sizeof side, "%s-journal", tmp);
    file_remove(side);
    sqlite3 *src = g_db, *dst = NULL;
    if (sqlite3_open_v2(tmp, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        log_error("store: couldn't make %s: %s", tmp, dst ? sqlite3_errmsg(dst) : "out of memory");
        sqlite3_close(dst);
        return false;
    }
    g_db = dst;
    db_exec("PRAGMA journal_mode=DELETE"); /* (one file: it simply takes hemma.db's place) */
    char line[48];
    snprintf(line, sizeof line, "PRAGMA user_version = %d", (int)sql_int(src, "PRAGMA user_version"));
    db_exec(line); /* the data's format comes along: migrations past it still run, the others don't again */
    long long rows = 0, floor = sql_int(src, "SELECT v FROM store_meta WHERE k = 'min_reader'");
    if (floor) meta_set("min_reader", floor);
    Buf lost = {0}; /* "note, booking, pref and 4 more tables" */
    int nlost = 0;
    bool ok = true;
    db_begin();
    for (int i = 0; i < ntables && ok; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        bool l = false;
        ok = ensure_schema(t) && prepare_stmts(t);
        if (ok) rescue_table(src, t, &rows, &l);
        if (l && ++nlost <= 3) buf_printf(&lost, "%s%s", lost.len ? ", " : "", t->name);
        unprepare(t);
    }
    if (nlost > 3) buf_printf(&lost, " and %d more table%s", nlost - 3, nlost - 3 == 1 ? "" : "s");
    db_commit();
    sqlite3_close(dst);
    g_db = src;
    if (!ok) {
        file_remove(tmp);
        buf_free(&lost);
        return false;
    }
    sqlite3_close(src);
    g_db = NULL;
    file_remove(path);
    snprintf(side, sizeof side, "%s-wal", path);
    file_remove(side);
    snprintf(side, sizeof side, "%s-shm", path);
    file_remove(side);
    if (!file_replace(tmp, path)) {
        log_error("store: couldn't put the rescued database in place: it's %s - rename it to %s", tmp, path);
        buf_free(&lost);
        return false;
    }
    const char *base = strrchr(kept, '/'); /* (SQLite's own words go to the log; people get these) */
    snprintf(g_rescue, sizeof g_rescue,
             "%s %s: the database was damaged (%s). The damaged file is kept as %s. %lld row%s rescued into a fresh "
             "one; %s%s.",
             day, hm, strstr(why, "not a database") ? "it isn't a database file any more" : "parts of it can't be read",
             base ? base + 1 : kept, rows, rows == 1 ? " was" : "s were",
             lost.len ? "rows were lost in: " : "every table could be read to its end", buf_cstr(&lost));
    buf_free(&lost);
    return true;
}

/* =========================================================== open / close */

static bool open_file(const char *path) {
    if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        log_error("cannot open %s: %s", path, g_db ? sqlite3_errmsg(g_db) : "out of memory");
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }
    sqlite3_busy_timeout(g_db, 3000);
    return true;
}

static void close_file(void) {
    if (g_db) sqlite3_close_v2(g_db);
    g_db = NULL;
    g_tx_depth = 0;
}

/* Every table empty and unprepared again (registrations stay). */
static void unload_tables(void) {
    for (int i = 0; i < ntables; i++) {
        Table *t = tables[i];
        unprepare(t);
        free(t->rows);
        t->rows = NULL;
        t->n = t->cap = 0;
        t->next_id = 1;
    }
}

/* 1 = open and loaded, 0 = damaged, -1 = can't be used (another failure, a newer format). */
static int open_tables(const char *path) {
    db_exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;");
    int have = 0;
    bool risky = false;
    if (!format_ok(path, &have, &risky)) return -1;
    bool upgrade = have < store_format();
    if (upgrade && risky && sql_int(g_db, "SELECT count(*) FROM sqlite_master WHERE type = 'table'") > 0) snapshot(path);
    int result = 1;
    long long rows = 0;
    db_begin();
    for (int i = 0; i < ntables && result > 0; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        if (!ensure_schema(t) || !prepare_stmts(t)) result = damage(sqlite3_errcode(g_db)) ? 0 : -1;
    }
    if (result > 0 && upgrade && !migrate(have)) result = -1;
    for (int i = 0; i < ntables && result > 0; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        result = load(t);
        rows += t->n;
    }
    if (result > 0) {
        db_commit();
        log_info("store: %lld rows from %s (SQLite %s)", rows, path, sqlite3_libversion());
    } else {
        g_tx_depth = 0; /* (nothing half-changed stays behind) */
        db_exec("ROLLBACK");
    }
    return result;
}

bool store_open(const char *dir) {
    if (!dir_make(dir)) {
        log_error("cannot create data directory %s", dir);
        return false;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    g_rescue[0] = '\0';
    for (int attempt = 0;; attempt++) {
        if (!open_file(path)) return false;
        char why[200];
        int verdict = check_file(why, sizeof why);
        if (verdict < 0) {
            log_error("cannot read %s: %s", path, why);
            close_file();
            return false;
        }
        if (verdict == 0) {
            int r = open_tables(path);
            if (r > 0) return true;
            if (r < 0) {
                unload_tables();
                close_file();
                return false;
            }
            str_copy(why, sizeof why, sqlite3_errmsg(g_db));
        }
        unload_tables();
        if (attempt > 0) {
            log_error("store: %s is still damaged after the rescue (%s)", path, why);
            close_file();
            return false;
        }
        log_error("store: %s is damaged (%s): rescuing every row that can still be read", path, why);
        if (!rescue(path, why)) {
            close_file();
            return false;
        }
        log_error("store: %s", g_rescue);
    }
}

void store_maintain(void) {
    reset_cached();
    db_exec("PRAGMA optimize; PRAGMA wal_checkpoint(TRUNCATE);");
}

void store_close(void) {
    unload_tables();
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
    g_last_err = g_db ? sqlite3_extended_errcode(g_db) : 0;
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
