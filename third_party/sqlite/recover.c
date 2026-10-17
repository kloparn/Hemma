/* SQLite's recovery extension as one object: ext/recover/dbdata.c and ext/recover/sqlite3recover.c from
 * sqlite-src-3540100, unmodified (README.md). It reads a damaged database page by page - what the
 * sqlite3 shell's .recover does - so damage high in a table's tree, or a broken file header, doesn't
 * lose the rows that are still there (issues #87, #88). Needs sqlite3.c built with
 * SQLITE_ENABLE_DBPAGE_VTAB. */
#include "dbdata.c"
#include "sqlite3recover.c"
