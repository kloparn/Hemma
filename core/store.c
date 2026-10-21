/* core/store.c — typed in-memory tables persisted in SQLite (see store.h),
 * plus the shared connection for direct SQL (see db.h). */
#include "store.h"
#include "db.h"
#include "i18n.h"
#include "json.h"
#include "plat.h"
#include "tar.h"
#include "../third_party/sqlite/sqlite3recover.h"

#include <limits.h>

#define MAX_TABLES 64
#define DAMAGED_KEEP 3 /* damaged copies kept in the data folder: the newest few */
#define RESTORE_KEEP 3 /* ... and copies of the data from before a restore */

typedef struct {
    sqlite3_stmt *insert, *upsert, *del;
} Stmts;

static Table *tables[MAX_TABLES];
static int ntables;
static sqlite3 *g_db;
static bool g_failed;
static int g_last_err;     /* SQLite's code for the last write that failed */
static char g_rescue[700];          /* store_rescue_report(): in English, for the log */
static char g_rescue_packed[1400];  /* the same for people: packed (i18n_pack_add), shown in their language */
static char g_restored_packed[1400];
static bool g_refused;     /* store_refused(): the data was left alone, on purpose */
static char g_refusal[800]; /* store_refusal() */
static char g_restored[700]; /* store_restore_report() */
static char g_dir[600];    /* the data folder store_open was given */
static const char *g_program = ""; /* store_set_program(): this Hemma's version */
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
const char *store_rescue_packed(void) { return g_rescue_packed; }

/* packed -> the English text (the log's, the tests') */
static void english(char *out, size_t cap, const char *packed) {
    Lang was = i18n_use(LANG_EN);
    i18n_unpack_to(out, cap, packed);
    i18n_use(was);
}

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

/* "program" in store_meta: the version of the newest Hemma that opened this data (a refusal names it). */
static void meta_text(const char *k, char *out, size_t cap) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT v FROM store_meta WHERE k = ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_BLOB)
            snprintf(out, cap, "%.*s", sqlite3_column_bytes(st, 0), (const char *)sqlite3_column_blob(st, 0));
    }
    sqlite3_finalize(st);
}

static bool meta_set_text(const char *k, const char *v) {
    sqlite3_stmt *st = NULL;
    bool ok = db_exec("CREATE TABLE IF NOT EXISTS store_meta (k TEXT PRIMARY KEY, v INTEGER NOT NULL)") &&
              sqlite3_prepare_v2(g_db, "INSERT INTO store_meta (k, v) VALUES (?1, ?2) ON CONFLICT(k) DO UPDATE SET v = ?2",
                                 -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        /* (a blob: v is an INTEGER column, where text such as "1.2" would turn into a number) */
        sqlite3_bind_blob(st, 2, v, (int)strlen(v), SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    return ok;
}

/* Before anything is changed: false = a newer Hemma's data this one would misread. Issue #92: the
   refusal says it in versions people have - which Hemma last used the data, which one this is - and
   names the copy made before the change; store_refusal() keeps it for the data folder's log. */
static bool format_ok(const char *path, int *have, bool *risky) {
    *have = (int)sql_int(g_db, "PRAGMA user_version");
    int mine = store_format(), floor = (int)sql_int(g_db, "SELECT v FROM store_meta WHERE k = 'min_reader'");
    if (*have > mine && floor > mine) {
        char by[40], back[720];
        meta_text("program", by, sizeof by);
        snprintf(back, sizeof back, "%s.before-format-%d", path, *have);
        bool copy = path_stat(back, NULL, NULL, NULL);
        const char *base = strrchr(back, '/');
        snprintf(g_refusal, sizeof g_refusal,
                 "This data was last used by %s%s (data format %d), and this Hemma%s%s reads only up to format %d - "
                 "it would misread it, so it won't start. Install %s%s again%s%s.",
                 by[0] ? "Hemma " : "a newer Hemma", by, *have, g_program[0] ? " " : "", g_program, mine,
                 by[0] ? "Hemma " : "the newer Hemma", by[0] ? by : "", copy ? ", or put back the copy made before "
                                                                              "the change: " : "",
                 copy ? (base ? base + 1 : back) : "");
        if (by[0]) { /* "Install Hemma 1.2 or newer again" */
            char *at = strstr(g_refusal, " again");
            if (at && (size_t)(at - g_refusal) + 16 < sizeof g_refusal) {
                memmove(at + 9, at, strlen(at) + 1);
                memcpy(at, " or newer", 9);
            }
        }
        log_error("store: %s: %s", path, g_refusal);
        g_refused = true;
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

static void unprepare(Table *t) {
    Stmts *s = t->impl;
    if (!s) return;
    sqlite3_finalize(s->insert);
    sqlite3_finalize(s->upsert);
    sqlite3_finalize(s->del);
    free(s);
    t->impl = NULL;
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
    if (!ok) file_remove(to); /* (half a copy is no copy: issue #88 - a full disk left them on every start) */
    return ok;
}

/* ---- the census: how many rows each table had at the last healthy start ----
   Kept beside the database (hemma.db.counts), not in it: a rescue says what it kept against it
   ("notes on the board: 2,944 of 3,000") and sees when it got next to nothing back (issues #87, #88). */

static void census_path(char *out, size_t cap) { snprintf(out, cap, "%s/hemma.db.counts", g_dir); }

static void census_write(void) {
    if (!g_dir[0]) return;
    char path[640];
    census_path(path, sizeof path);
    Buf b = {0};
    for (int i = 0; i < ntables; i++)
        if (!tables[i]->memory_only) buf_printf(&b, "%s %d\n", tables[i]->name, tables[i]->n);
    file_write_atomic(path, b.p ? b.p : "", b.len);
    buf_free(&b);
}

/* The census's rows for a table, -1 = not known (no census yet, or the table is new). */
static long long census_of(const Buf *census, const char *table) {
    size_t n = strlen(table);
    for (const char *p = census->p ? census->p : ""; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > n + 1 && memcmp(p, table, n) == 0 && p[n] == ' ') return strtoll(p + n + 1, NULL, 10);
        p = *end ? end + 1 : end;
    }
    return -1;
}

/* "2,944" - or, in a text shown in any language (plain = true), "2944" (and "12 345" past 9,999: a
   comma reads as a decimal point in Swedish and Spanish) */
static void count_text_in(long long v, char *out, size_t cap, bool plain) {
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", v < 0 ? 0 : v);
    size_t len = strlen(raw), o = 0;
    for (size_t i = 0; i < len && o + 2 < cap; i++) {
        if (i && (len - i) % 3 == 0 && (!plain || len > 4)) out[o++] = plain ? ' ' : ',';
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

static void count_text(long long v, char *out, size_t cap) { count_text_in(v, out, cap, false); }

/* ---- copies kept beside the database (damaged ones, the data before a restore): the newest few stay ---- */

typedef struct {
    char name[200];
} Kept;

static int kept_cmp(const void *a, const void *b) { return strcmp(((const Kept *)a)->name, ((const Kept *)b)->name); }

static void prune_copies(const char *prefix, int keep) {
    DirIter *it = dir_open(g_dir);
    DirEntry e;
    Kept *list = NULL;
    int n = 0, cap = 0;
    while (it && dir_next(it, &e)) {
        if (!str_starts(e.name, prefix) || str_ends(e.name, "-wal") || strlen(e.name) >= sizeof list->name)
            continue;
        if (n == cap) list = xrealloc(list, (size_t)(cap = cap ? cap * 2 : 8) * sizeof *list);
        str_copy(list[n++].name, sizeof list->name, e.name);
    }
    dir_close(it);
    if (n > keep) {
        qsort(list, (size_t)n, sizeof *list, kept_cmp); /* (names carry the day and time: oldest first) */
        for (int i = 0; i < n - keep; i++) {
            char path[900];
            snprintf(path, sizeof path, "%s/%s", g_dir, list[i].name);
            file_remove(path);
            snprintf(path, sizeof path, "%s/%s-wal", g_dir, list[i].name);
            file_remove(path);
            log_info("store: removed an older copy, %s (the newest %d stay)", list[i].name, keep);
        }
    }
    free(list);
}

/* ---- the rescue ---- */

typedef enum { RESCUED, RESCUE_REFUSED, RESCUE_FAILED } Rescue;

/* What `what` a table holds, for people: its label (Table.what) or its name. */
static const char *table_words(const Table *t) { return t->what && t->what[0] ? t->what : t->name; }

static long long count_rows(sqlite3 *d, const char *table) {
    char *sql = sqlite3_mprintf("SELECT count(*) FROM \"%w\"", table);
    long long n = sql ? sql_int(d, sql) : 0;
    sqlite3_free(sql);
    return n;
}

/* SQLite's own recovery (third_party/sqlite/recover.c, what the shell's .recover does): it reads the
   damaged file page by page - a broken header, a file cut short, damage high in a table's tree - and
   writes every row it can still place into a fresh database at `to`. A plain SELECT stops at the
   first broken page of a table's tree and loses everything under it (issue #88: 0 of 3,000 notes
   where .recover got all of them back). */
static bool recover_into(sqlite3 *src, const char *to, char *err, size_t cap) {
    char side[720];
    file_remove(to);
    snprintf(side, sizeof side, "%s-journal", to);
    file_remove(side);
    sqlite3_recover *p = sqlite3_recover_init(src, "main", to);
    if (!p) {
        str_copy(err, cap, "out of memory");
        return false;
    }
    sqlite3_recover_config(p, SQLITE_RECOVER_LOST_AND_FOUND, (void *)"lost_and_found"); /* (place_orphans) */
    sqlite3_recover_run(p);
    if (sqlite3_recover_errcode(p) != SQLITE_OK) str_copy(err, cap, sqlite3_recover_errmsg(p));
    return sqlite3_recover_finish(p) == SQLITE_OK;
}

/* The table's columns in `d`, in order: 'T' text, 'I' integer (Hemma declares nothing else); 0 = no
   such table there. */
static int column_kinds(sqlite3 *d, const char *table, char *out, int cap) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(d, "SELECT type FROM pragma_table_info(?1) ORDER BY cid", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW && n < cap) {
            const char *ty = (const char *)sqlite3_column_text(st, 0);
            out[n++] = ty && strstr(ty, "TEXT") ? 'T' : 'I';
        }
    }
    sqlite3_finalize(st);
    return n;
}

typedef struct {
    Table *t; /* NULL: a table of the file that isn't one of ours (a cache) - never placed into */
    char kinds[128];
    int nk;
    sqlite3_stmt *taken; /* SELECT 1 ... WHERE rowid = ?1 */
    sqlite3_stmt *ins;   /* the INSERT for rows of ins_nfield fields */
    int ins_nfield;
} Home;

static bool home_open(sqlite3 *d, Home *h, Table *t, const char *name) {
    memset(h, 0, sizeof *h);
    h->t = t;
    h->nk = column_kinds(d, name, h->kinds, (int)sizeof h->kinds);
    char *sql = h->nk > 0 ? sqlite3_mprintf("SELECT 1 FROM \"%w\" WHERE rowid = ?1", name) : NULL;
    bool ok = sql && sqlite3_prepare_v2(d, sql, -1, &h->taken, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    return ok;
}

/* Does every row of the page (rows: SELECT * FROM lost_and_found WHERE pgno = ...) fit h's table? */
static bool page_fits(sqlite3_stmt *rows, int ncols, Home *h) {
    bool ok = true, any = false;
    sqlite3_reset(rows);
    while (ok && sqlite3_step(rows) == SQLITE_ROW) {
        any = true;
        int nfield = sqlite3_column_int(rows, 2);
        ok = nfield >= 1 && nfield <= h->nk && 4 + nfield <= ncols && sqlite3_column_type(rows, 4) == SQLITE_NULL;
        for (int j = 1; ok && j < nfield; j++) {
            int ty = sqlite3_column_type(rows, 4 + j);
            ok = h->kinds[j] == 'T' ? ty == SQLITE_TEXT : ty == SQLITE_INTEGER;
        }
        if (ok) { /* (an id that's taken: another table's row, or a copy) */
            sqlite3_reset(h->taken);
            sqlite3_bind_int64(h->taken, 1, sqlite3_column_int64(rows, 3));
            ok = sqlite3_step(h->taken) != SQLITE_ROW;
        }
    }
    return ok && any;
}

static long long put_page(sqlite3 *d, sqlite3_stmt *rows, Home *h) {
    long long placed = 0;
    sqlite3_reset(rows);
    while (sqlite3_step(rows) == SQLITE_ROW) {
        int nfield = sqlite3_column_int(rows, 2);
        if (!h->ins || h->ins_nfield != nfield) {
            sqlite3_finalize(h->ins);
            h->ins = NULL;
            Buf sql = {0};
            buf_printf(&sql, "INSERT INTO \"%s\" SELECT ?1", h->t->name); /* (the id, then the row's own values) */
            for (int j = 1; j < nfield; j++) buf_printf(&sql, ", ?%d", j + 1);
            for (int j = nfield; j < h->nk; j++) buf_puts(&sql, h->kinds[j] == 'T' ? ", ''" : ", 0"); /* (columns added later) */
            if (sqlite3_prepare_v2(d, buf_cstr(&sql), -1, &h->ins, NULL) == SQLITE_OK) h->ins_nfield = nfield;
            buf_free(&sql);
            if (!h->ins) continue;
        }
        sqlite3_reset(h->ins);
        sqlite3_bind_int64(h->ins, 1, sqlite3_column_int64(rows, 3));
        for (int j = 1; j < nfield; j++) sqlite3_bind_value(h->ins, j + 1, sqlite3_column_value(rows, 4 + j));
        if (sqlite3_step(h->ins) == SQLITE_DONE) placed++;
    }
    return placed;
}

/* Rows SQLite's recovery couldn't place in a table go to `lost_and_found` (rootpgno, pgno, nfield,
   id, c0, c1 ...): their tree's page above them was the damaged one. Every row on one page belongs
   to one table, so a page goes back where its rows fit exactly one table - the first column empty
   (the id lives in the rowid), the rest text or integer where the table has text or integer, no id
   that's taken - and that table is one of ours. The file's other tables with an integer id (none
   today: media_file's id is text) count as places a page may fit too, so a cache's page never
   lands in the household's data. Pages that fit none or more than one stay in lost_and_found (in
   the rescued file, not thrown away). Returns the rows placed; *left = the rows still there,
   *unsure = those of them on pages that fit one of ours and something else: the household's rows
   Hemma couldn't tell apart. The rest (a cache's, most likely - Hemma rebuilds those) is only logged. */
static long long place_orphans(sqlite3 *d, long long *left, long long *unsure) {
    *left = *unsure = 0;
    char probe[8];
    if (column_kinds(d, "lost_and_found", probe, (int)sizeof probe) < 4) return 0; /* (nothing was lost) */
    int cap = ntables + 8, nh = 0;
    Home *homes = xmalloc((size_t)cap * sizeof *homes);
    for (int i = 0; i < ntables; i++)
        if (!tables[i]->memory_only && home_open(d, &homes[nh], tables[i], tables[i]->name)) nh++;
    sqlite3_stmt *others = NULL;
    sqlite3_prepare_v2(d,
                       "SELECT m.name FROM sqlite_master m, pragma_table_info(m.name) c WHERE m.type = 'table' AND "
                       "m.name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND m.name <> 'lost_and_found' AND m.sql NOT LIKE "
                       "'%WITHOUT ROWID%' AND c.cid = 0 AND c.pk = 1 AND upper(c.type) = 'INTEGER'",
                       -1, &others, NULL);
    while (others && sqlite3_step(others) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(others, 0);
        if (!name || store_table(name)) continue;
        if (nh == cap) homes = xrealloc(homes, (size_t)(cap *= 2) * sizeof *homes);
        if (home_open(d, &homes[nh], NULL, name)) nh++;
    }
    sqlite3_finalize(others);
    long long placed = 0, *pgs = NULL;
    int npg = 0, cappg = 0;
    sqlite3_stmt *pages = NULL; /* (all of them first: placing a page deletes its rows) */
    sqlite3_prepare_v2(d, "SELECT DISTINCT pgno FROM lost_and_found ORDER BY pgno", -1, &pages, NULL);
    while (pages && sqlite3_step(pages) == SQLITE_ROW) {
        if (npg == cappg) pgs = xrealloc(pgs, (size_t)(cappg = cappg ? cappg * 2 : 16) * sizeof *pgs);
        pgs[npg++] = sqlite3_column_int64(pages, 0);
    }
    sqlite3_finalize(pages);
    sqlite3_stmt *rows = NULL, *del = NULL, *count = NULL;
    sqlite3_prepare_v2(d, "SELECT * FROM lost_and_found WHERE pgno = ?1", -1, &rows, NULL);
    sqlite3_prepare_v2(d, "DELETE FROM lost_and_found WHERE pgno = ?1", -1, &del, NULL);
    sqlite3_prepare_v2(d, "SELECT count(*) FROM lost_and_found WHERE pgno = ?1", -1, &count, NULL);
    sqlite3_exec(d, "BEGIN", NULL, NULL, NULL);
    for (int pi = 0; rows && del && pi < npg; pi++) {
        sqlite3_reset(rows);
        sqlite3_bind_int64(rows, 1, pgs[pi]);
        int ncols = sqlite3_column_count(rows); /* rootpgno, pgno, nfield, id, then c0 ... */
        Home *home = NULL;
        int fits = 0, ours = 0;
        for (int i = 0; i < nh; i++)
            if (page_fits(rows, ncols, &homes[i])) home = &homes[i], fits++, ours += homes[i].t != NULL;
        if (fits != 1 || !ours) {
            if (ours && count) {
                sqlite3_reset(count);
                sqlite3_bind_int64(count, 1, pgs[pi]);
                if (sqlite3_step(count) == SQLITE_ROW) *unsure += sqlite3_column_int64(count, 0);
            }
            continue;
        }
        placed += put_page(d, rows, home);
        sqlite3_reset(rows);
        sqlite3_reset(del);
        sqlite3_bind_int64(del, 1, pgs[pi]);
        sqlite3_step(del);
    }
    sqlite3_exec(d, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(rows);
    sqlite3_finalize(del);
    sqlite3_finalize(count);
    for (int i = 0; i < nh; i++) {
        sqlite3_finalize(homes[i].taken);
        sqlite3_finalize(homes[i].ins);
    }
    free(homes);
    free(pgs);
    *left = sql_int(d, "SELECT count(*) FROM lost_and_found");
    if (*left == 0) sqlite3_exec(d, "DROP TABLE lost_and_found", NULL, NULL, NULL);
    return placed;
}

/* ids are never handed out twice: each table's sequence goes past the old one (when it can still be
   read) and past every id kept - with rows lost and no old sequence, 1,000 past. */
static void keep_sequences(sqlite3 *src, sqlite3 *dst, bool lost) {
    for (int i = 0; i < ntables; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        char *sql = sqlite3_mprintf("SELECT max(\"id\") FROM \"%w\"", t->name);
        long long top = sql ? sql_int(dst, sql) : 0, seq = -1;
        sqlite3_free(sql);
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(src, "SELECT seq FROM sqlite_sequence WHERE name = ?1", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, t->name, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW) seq = sqlite3_column_int64(q, 0);
        }
        sqlite3_finalize(q);
        if (seq < top) seq = top + (lost ? 1000 : 0);
        if (seq <= 0 || seq >= INT_MAX) continue;
        sql = sqlite3_mprintf("DELETE FROM sqlite_sequence WHERE name = %Q;"
                              "INSERT INTO sqlite_sequence (name, seq) VALUES (%Q, %lld);",
                              t->name, t->name, seq);
        if (sql) sqlite3_exec(dst, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
    }
}

/* The damaged file is copied aside untouched (hemma.db.damaged-<when>, its WAL too), and SQLite's
   recovery reads what it can into a fresh file. Then the verdict (issue #87): next to nothing back -
   no rows at all, none of the household's accounts, or under a tenth of what the census says there
   was - is no rescue: Hemma would start as a brand-new household over the real data (the welcome
   page, "set it up again"). The file stays exactly as it was and Hemma won't start (RESCUE_REFUSED,
   store_refusal says why). Otherwise the fresh file takes its place and store_rescue_report says,
   in the household's words, what was kept. g_db is the damaged one. */
static Rescue rescue(const char *path, const char *why) {
    char day[11], hm[6], kept[700], kept_wal[720], tmp[700], side[720];
    int64_t now = now_sec();
    fmt_ymd(now, day);
    fmt_hm(now, hm);
    snprintf(kept, sizeof kept, "%s.damaged-%s-%c%c%c%c", path, day, hm[0], hm[1], hm[3], hm[4]);
    for (int k = 2; k < 100 && path_stat(kept, NULL, NULL, NULL); k++)
        snprintf(kept, sizeof kept, "%s.damaged-%s-%c%c%c%c-%d", path, day, hm[0], hm[1], hm[3], hm[4], k);
    snprintf(side, sizeof side, "%s-wal", path);
    snprintf(kept_wal, sizeof kept_wal, "%s-wal", kept);
    if (!copy_file(path, kept) || !copy_file(side, kept_wal)) {
        file_remove(kept);
        file_remove(kept_wal);
        log_error("store: couldn't keep a copy of the damaged %s as %s - not touching it (is the disk full?)", path, kept);
        return RESCUE_FAILED;
    }
    prune_copies("hemma.db.damaged-", DAMAGED_KEEP);
    snprintf(tmp, sizeof tmp, "%s.rescue", path);
    char err[200] = "";
    if (!recover_into(g_db, tmp, err, sizeof err))
        log_warn("store: the recovery stopped early (%s) - going on with what it got", err[0] ? err : "?");
    sqlite3 *dst = NULL;
    if (sqlite3_open_v2(tmp, &dst, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        log_error("store: couldn't open what the recovery wrote (%s): %s", tmp, dst ? sqlite3_errmsg(dst) : "?");
        sqlite3_close(dst);
        file_remove(tmp);
        return RESCUE_FAILED;
    }
    long long left = 0, orphans = 0, placed = place_orphans(dst, &left, &orphans);
    if (placed || left)
        log_info("store: %lld rows the recovery couldn't place went back to their tables; left in lost_and_found: %lld "
                 "that fit more than one and %lld that fit none (a cache's, most likely)",
                 placed, orphans, left - orphans);
    /* what came back, table by table, against the census */
    char cpath[640];
    census_path(cpath, sizeof cpath);
    Buf census = {0};
    bool have_census = file_read_all(cpath, &census, 1 << 20);
    long long kept_rows = 0, was_rows = 0, people = -1, was_people = -1;
    Buf lost = {0}; /* "notes on the board: 2,944 of 3,000, bookings: 12 of 40 and 2 more" */
    int nlost = 0;
    bool unknown = !have_census;
    for (int i = 0; i < ntables; i++) {
        Table *t = tables[i];
        if (t->memory_only) continue;
        long long n = count_rows(dst, t->name), was = have_census ? census_of(&census, t->name) : -1;
        kept_rows += n;
        if (was >= 0) was_rows += was;
        else unknown = true;
        if (str_eq(t->name, "person")) people = n, was_people = was;
        if (was > n && ++nlost <= 3) {
            char a[24], b2[24];
            count_text_in(n, a, sizeof a, true);
            count_text_in(was, b2, sizeof b2, true);
            buf_printf(&lost, "%s%s: %s of %s", lost.len ? ", " : "", table_words(t), a, b2);
        }
    }
    if (nlost > 3) buf_printf(&lost, " and %d more", nlost - 3);
    buf_free(&census);
    char k[24], w[24];
    count_text(kept_rows, k, sizeof k);
    count_text(was_rows, w, sizeof w);
    const char *nothing = NULL;
    if (kept_rows == 0) nothing = "nothing";
    else if (people == 0 && was_people != 0) nothing = "none of the household's accounts";
    else if (have_census && was_rows >= 20 && kept_rows * 10 < was_rows) nothing = "next to nothing";
    if (nothing) {
        sqlite3_close(dst);
        file_remove(tmp);
        file_remove(kept); /* (the file itself stays as it was: the copy would only be one more of it) */
        file_remove(kept_wal);
        snprintf(g_refusal, sizeof g_refusal,
                 "Hemma's data in %s is damaged (%s), and a rescue got %s back (%s%s%s rows). Hemma won't start on "
                 "it and changed nothing - starting as a new household would build over your data. Get it back "
                 "from a backup, or try SQLite's own .recover on a copy (docs/troubleshooting.md, \"A damaged "
                 "database\"); to really start over, move hemma.db away first.",
                 path, strstr(why, "not a database") ? "it isn't a database file any more" : "parts of it can't be read",
                 nothing, k, have_census && was_rows ? " of " : "", have_census && was_rows ? w : "");
        g_refused = true;
        buf_free(&lost);
        return RESCUE_REFUSED;
    }
    keep_sequences(g_db, dst, nlost > 0 || unknown);
    char line[48];
    snprintf(line, sizeof line, "PRAGMA user_version = %d", (int)sql_int(g_db, "PRAGMA user_version"));
    if (sql_int(dst, "PRAGMA user_version") == 0) sqlite3_exec(dst, line, NULL, NULL, NULL);
    sqlite3_close(dst);
    sqlite3_close(g_db);
    g_db = NULL;
    file_remove(path);
    snprintf(side, sizeof side, "%s-wal", path);
    file_remove(side);
    snprintf(side, sizeof side, "%s-shm", path);
    file_remove(side);
    if (!file_replace(tmp, path)) {
        log_error("store: couldn't put the rescued database in place: it's %s - rename it to %s", tmp, path);
        buf_free(&lost);
        return RESCUE_FAILED;
    }
    const char *base = strrchr(kept, '/'); /* (SQLite's own words go to the log; people get these) */
    char *r = g_rescue_packed;
    r[0] = '\0';
    if (strstr(why, "not a database"))
        i18n_pack_add(r, sizeof g_rescue_packed,
                      N_("%s %s: Hemma's data was damaged (it isn't a database file any more). Nothing was deleted: the "
                         "damaged file is kept as %s."),
                      day, hm, base ? base + 1 : kept);
    else
        i18n_pack_add(r, sizeof g_rescue_packed,
                      N_("%s %s: Hemma's data was damaged (parts of it couldn't be read). Nothing was deleted: the "
                         "damaged file is kept as %s."),
                      day, hm, base ? base + 1 : kept);
    if (unknown && lost.len)
        i18n_pack_add(r, sizeof g_rescue_packed, N_("%'lld rows were kept; fewer than before in %s."), kept_rows,
                      buf_cstr(&lost));
    else if (unknown) i18n_pack_add(r, sizeof g_rescue_packed, N_("%'lld rows were kept."), kept_rows);
    else if (lost.len)
        i18n_pack_add(r, sizeof g_rescue_packed, N_("%'lld of %'lld rows were kept - %s."), kept_rows, was_rows,
                      buf_cstr(&lost));
    else i18n_pack_add(r, sizeof g_rescue_packed, N_("All %'lld rows were kept."), kept_rows);
    if (orphans > 0)
        i18n_pack_add(r, sizeof g_rescue_packed,
                      N_("%'lld more couldn't be put back in their place: they're in the table lost_and_found."),
                      (long long)orphans);
    english(g_rescue, sizeof g_rescue, r);
    buf_free(&lost);
    return RESCUED;
}

/* ====================================================== backups (issue #35) */

/* Rows of Hemma's own tables in d - those with an integer id, as every store table has - and the
   people among them (-1: no person table). Works without the tables registered: hemma --restore
   checks a backup before any module is loaded. */
static long long hemma_rows(sqlite3 *d, long long *people) {
    long long n = 0;
    *people = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d,
                           "SELECT m.name FROM sqlite_master m, pragma_table_info(m.name) c WHERE m.type = 'table' AND "
                           "m.name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND m.name <> 'lost_and_found' AND c.name = 'id' "
                           "AND c.pk = 1 AND upper(c.type) = 'INTEGER'",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (!t) continue;
        long long k = count_rows(d, t);
        n += k;
        if (str_eq(t, "person")) *people = k;
    }
    sqlite3_finalize(st);
    return n;
}

bool store_check_copy(const char *path, long long *rows, char *why, size_t cap) {
    *rows = 0;
    if (cap) why[0] = '\0';
    sqlite3 *d = NULL;
    bool ok = path_stat(path, NULL, NULL, NULL) && sqlite3_open_v2(path, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK;
    if (!ok) str_copy(why, cap, d ? sqlite3_errmsg(d) : "it isn't there");
    if (ok) {
        sqlite3_busy_timeout(d, 3000);
        char first[200] = "";
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(d, "PRAGMA quick_check", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
            sqlite3_column_text(st, 0))
            str_copy(first, sizeof first, (const char *)sqlite3_column_text(st, 0));
        else str_copy(first, sizeof first, sqlite3_errmsg(d));
        sqlite3_finalize(st);
        if (!str_eq(first, "ok")) {
            ok = false;
            snprintf(why, cap, "it's damaged (%s)", first);
        }
    }
    long long people = -1;
    if (ok) {
        *rows = hemma_rows(d, &people);
        if (people < 0) {
            ok = false;
            str_copy(why, cap, "it isn't Hemma's data");
        }
    }
    if (ok) { /* (a file only a newer Hemma reads: it would refuse to start on it) */
        int floor = (int)sql_int(d, "SELECT v FROM store_meta WHERE k = 'min_reader'");
        if (floor > store_format()) {
            ok = false;
            snprintf(why, cap, "a newer Hemma made it (data format %d - this one reads up to %d)", floor, store_format());
        }
    }
    sqlite3_close(d);
    return ok;
}

bool store_backup(const char *to, long long *rows, char *why, size_t cap) {
    *rows = 0;
    char src[640], part[720];
    snprintf(src, sizeof src, "%s/hemma.db", g_dir);
    snprintf(part, sizeof part, "%s.part", to);
    if (!g_dir[0] || !path_stat(src, NULL, NULL, NULL)) {
        str_copy(why, cap, "there's no database open");
        return false;
    }
    file_remove(part);
    /* its own connection: any thread, and the household's changes go on meanwhile (WAL) */
    sqlite3 *d = NULL;
    bool ok = sqlite3_open_v2(src, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_busy_timeout(d, 10000);
        char *sql = sqlite3_mprintf("VACUUM INTO %Q", part);
        ok = sql && sqlite3_exec(d, sql, NULL, NULL, NULL) == SQLITE_OK;
        sqlite3_free(sql);
    }
    if (!ok) snprintf(why, cap, "the copy failed: %s", d ? sqlite3_errmsg(d) : "out of memory");
    sqlite3_close(d);
    if (ok) { /* (VACUUM INTO doesn't make sure what it wrote reached the disk) */
        FILE *f = file_open(part, "r+b");
        ok = f && file_sync(f);
        if (f) fclose(f);
        if (!ok) str_copy(why, cap, "the copy couldn't be written to the disk");
    }
    char check[200];
    if (ok && !store_check_copy(part, rows, check, sizeof check)) {
        ok = false;
        snprintf(why, cap, "the copy doesn't check out: %s", check);
    }
    if (ok && !file_replace(part, to)) {
        ok = false;
        str_copy(why, cap, "the finished copy couldn't be given its name");
    }
    if (!ok) file_remove(part);
    return ok;
}

static void staged_paths(const char *data_dir, char staged[640], char from[660]) {
    snprintf(staged, 640, "%s/hemma.db.restore", data_dir);
    snprintf(from, 660, "%s/hemma.db.restore.from", data_dir);
}

bool store_stage_restore(const char *data_dir, const char *file, const char *label, char *secret, size_t secret_cap,
                         char *why, size_t cap) {
    char staged[640], from[660], part[700];
    staged_paths(data_dir, staged, from);
    snprintf(part, sizeof part, "%s.part", staged);
    if (secret_cap) secret[0] = '\0';
    int64_t size = 0;
    bool dir = false;
    if (!path_stat(file, &size, NULL, &dir) || dir) {
        snprintf(why, cap, "there's no file at %s", file);
        return false;
    }
    if (!dir_make(data_dir)) {
        snprintf(why, cap, "can't make the data folder %s", data_dir);
        return false;
    }
    /* a download (.tar: the database and the password secret) or a plain backup (.db) */
    char head[512];
    size_t got = 0;
    FILE *f = file_open(file, "rb");
    if (f) {
        got = fread(head, 1, sizeof head, f);
        fclose(f);
    }
    bool ok;
    if (got == sizeof head && tar_looks(head, got)) {
        Buf t = {0};
        const void *p = NULL;
        size_t n = 0;
        ok = file_read_all(file, &t, (size_t)1 << 30) && tar_find(t.p, t.len, "hemma.db", &p, &n);
        if (!ok) snprintf(why, cap, "%s holds no hemma.db", file);
        if (ok && !(ok = file_write_atomic(part, p, n))) snprintf(why, cap, "can't write %s (is the disk full?)", part);
        const void *s = NULL;
        size_t sn = 0;
        if (ok && secret_cap && tar_find(t.p, t.len, "hemma-secret.txt", &s, &sn)) {
            const char *txt = s;
            for (size_t i = 0; i + 16 <= sn; i++) { /* (the HEMMA_AUTH_SALT= line) */
                if ((i && txt[i - 1] != '\n') || memcmp(txt + i, "HEMMA_AUTH_SALT=", 16) != 0) continue;
                size_t k = i + 16;
                while (k < sn && txt[k] != '\n' && txt[k] != '\r') k++;
                snprintf(secret, secret_cap, "%.*s", (int)(k - i - 16), txt + i + 16);
                break;
            }
        }
        buf_free(&t);
    } else {
        ok = copy_file(file, part);
        if (!ok) snprintf(why, cap, "can't copy %s into %s (is the disk full?)", file, data_dir);
    }
    long long rows = 0;
    char check[200];
    if (ok && !store_check_copy(part, &rows, check, sizeof check)) {
        ok = false;
        snprintf(why, cap, "that backup can't be used: %s", check);
    }
    if (ok && !(ok = file_replace(part, staged))) snprintf(why, cap, "can't put it in place as %s", staged);
    if (ok) file_write_atomic(from, label, strlen(label));
    if (!ok) file_remove(part);
    return ok;
}

bool store_restore_waiting(const char *data_dir, char *label, size_t cap) {
    char staged[640], from[660];
    staged_paths(data_dir, staged, from);
    if (cap) label[0] = '\0';
    if (!path_stat(staged, NULL, NULL, NULL)) return false;
    Buf b = {0};
    if (file_read_all(from, &b, 4096) && b.len) str_copy(label, cap, buf_cstr(&b));
    buf_free(&b);
    return true;
}

void store_unstage_restore(const char *data_dir) {
    char staged[640], from[660];
    staged_paths(data_dir, staged, from);
    file_remove(staged);
    file_remove(from);
}

/* At the start, before the database opens: a backup waiting as hemma.db.restore takes its place,
   checked once more; what was there is kept as hemma.db.before-restore-<day>-<hhmm> (the newest
   RESTORE_KEEP stay). One that doesn't check out is set aside (hemma.db.restore-refused) and the
   data stays as it was. store_restore_report() says which. */
static void restore_staged(const char *path) {
    char staged[640], from[660], kept[700], kept_wal[720], side[720], why[200] = "", label[200] = "";
    staged_paths(g_dir, staged, from);
    if (!store_restore_waiting(g_dir, label, sizeof label)) return;
    if (!label[0]) str_copy(label, sizeof label, "hemma.db.restore");
    char day[11], hm[6];
    int64_t now = now_sec();
    fmt_ymd(now, day);
    fmt_hm(now, hm);
    long long rows = 0;
    FILE *w = file_open(staged, "r+b"); /* (hemma --restore run as another user, root say: Hemma couldn't save to it) */
    if (w) fclose(w);
    else str_copy(why, sizeof why, "Hemma may not write to it - run hemma --restore as the user Hemma runs as");
    if (!w || !store_check_copy(staged, &rows, why, sizeof why)) {
        snprintf(kept, sizeof kept, "%s.restore-refused", path);
        file_replace(staged, kept);
        file_remove(from);
        g_restored_packed[0] = '\0';
        i18n_pack_add(g_restored_packed, sizeof g_restored_packed,
                      N_("%s %s: the backup %s was NOT put back - %s. Hemma carried on with the data it had (the backup "
                         "is kept as hemma.db.restore-refused)."),
                      day, hm, label, why);
        english(g_restored, sizeof g_restored, g_restored_packed);
        log_error("store: %s", g_restored);
        return;
    }
    bool had = path_stat(path, NULL, NULL, NULL);
    snprintf(kept, sizeof kept, "%s.before-restore-%s-%c%c%c%c", path, day, hm[0], hm[1], hm[3], hm[4]);
    for (int k = 2; k < 100 && path_stat(kept, NULL, NULL, NULL); k++)
        snprintf(kept, sizeof kept, "%s.before-restore-%s-%c%c%c%c-%d", path, day, hm[0], hm[1], hm[3], hm[4], k);
    snprintf(side, sizeof side, "%s-wal", path);
    snprintf(kept_wal, sizeof kept_wal, "%s-wal", kept);
    if (had && (!copy_file(path, kept) || !copy_file(side, kept_wal))) {
        file_remove(kept);
        file_remove(kept_wal);
        log_error("store: couldn't keep a copy of the data before restoring %s - not restoring it (is the disk "
                  "full?); it waits as %s",
                  label, staged);
        return;
    }
    file_remove(side);
    snprintf(side, sizeof side, "%s-shm", path);
    file_remove(side);
    if (!file_replace(staged, path)) {
        log_error("store: couldn't put the backup %s in place (%s) - Hemma carries on with the data it had", label, staged);
        return;
    }
    file_remove(from);
    prune_copies("hemma.db.before-restore-", RESTORE_KEEP);
    const char *base = strrchr(kept, '/');
    g_restored_packed[0] = '\0';
    i18n_pack_add(g_restored_packed, sizeof g_restored_packed, N_("%s %s: the backup %s was put back (%'lld rows)."), day,
                  hm, label, rows);
    if (had)
        i18n_pack_add(g_restored_packed, sizeof g_restored_packed, N_("What was there before is kept as %.200s."),
                      base ? base + 1 : kept);
    english(g_restored, sizeof g_restored, g_restored_packed);
    log_warn("store: %s", g_restored);
}

const char *store_restore_report(void) { return g_restored; }
const char *store_restore_packed(void) { return g_restored_packed; }

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
    if (result > 0 && have <= store_format() && g_program[0]) meta_set_text("program", g_program); /* (the newest one) */
    if (result > 0) {
        db_commit();
        log_info("store: %lld rows from %s (SQLite %s)", rows, path, sqlite3_libversion());
        census_write();
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
    str_copy(g_dir, sizeof g_dir, dir);
    g_rescue[0] = g_refusal[0] = g_restored[0] = g_rescue_packed[0] = g_restored_packed[0] = '\0';
    g_refused = false;
    restore_staged(path); /* (a backup an admin chose to go back to: issue #35) */
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
        Rescue r = rescue(path, why);
        if (r != RESCUED) {
            if (r == RESCUE_REFUSED) log_error("store: %s", g_refusal);
            close_file();
            return false;
        }
        log_error("store: %s", g_rescue);
    }
}

bool store_refused(void) { return g_refused; }
const char *store_refusal(void) { return g_refusal; }
void store_set_program(const char *version) { g_program = version ? version : ""; }

/* Issue #92: a rename (FIELD_WAS) with no breaks_older migration at or after its format leaves an
   older program re-adding the old column, empty - its rows look blank there. NULL = every rename is
   covered; else "table.field". */
const char *store_renames_unsafe(void) {
    static char bad[96];
    for (int i = 0; i < ntables; i++) {
        const Table *t = tables[i];
        for (int f = 0; f < t->nfields; f++) {
            const FieldDef *d = &t->fields[f];
            if (!d->was) continue;
            bool covered = false;
            for (int m = 0; m < g_nmigs && !covered; m++) covered = g_migs[m].breaks_older && g_migs[m].format >= d->was_format;
            if (d->was_format <= 0 || !covered) {
                snprintf(bad, sizeof bad, "%s.%s", t->name, d->name);
                return bad;
            }
        }
    }
    return NULL;
}

void store_maintain(void) {
    reset_cached();
    db_exec("PRAGMA optimize; PRAGMA wal_checkpoint(TRUNCATE);");
    if (g_db) census_write(); /* (what's in memory is what the household has) */
}

void store_close(void) {
    if (g_db && !g_failed) census_write(); /* (a clean stop: the counts a rescue would compare with) */
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
