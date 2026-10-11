/* core/db.h — direct SQLite access for modules that need real queries
 * (large libraries, history, statistics, full-text search via FTS5).
 *
 * One shared connection, used from the event-loop thread only. Worker
 * threads hand their results to the loop instead of touching the database.
 *
 *   sqlite3_stmt *st = db_prep("SELECT count(*) FROM booking WHERE res = ?");
 *   sqlite3_bind_int(st, 1, res);
 *   if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
 *   // no finalize/reset needed: db_prep() resets cached statements itself
 *
 * Every action runs inside a transaction (the core wraps it), so several
 * writes from one action land atomically. */
#ifndef HEMMA_DB_H
#define HEMMA_DB_H

#include "base.h"
#include "sqlite3.h"

sqlite3 *db(void);
bool db_exec(const char *sql);          /* one or more statements; logs errors */
/* Cached, already reset and unbound; NULL on error. One statement per SQL
   text: while stepping it, don't db_prep the same SQL again or db_commit
   (both reset it) - finish the loop first. */
sqlite3_stmt *db_prep(const char *sql);
void db_begin(void);                    /* nestable; only the outermost pair touches SQLite */
void db_commit(void);
void db_idle(void); /* core: after each loop turn, end lingering reads so the WAL can checkpoint */

#endif
