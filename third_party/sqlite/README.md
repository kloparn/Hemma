# SQLite (vendored)

- Version: 3.54.0 amalgamation (`sqlite3.c`, `sqlite3.h`), unmodified
- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
- SHA3-256 of the zip: `7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93`
  (matches the hash published on https://www.sqlite.org/download.html)
- SHA-256 of the files: `sqlite3.c` `2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243`,
  `sqlite3.h` `cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166`
- License: public domain — https://www.sqlite.org/copyright.html

Built once per target, with the compile options listed in the build files (WAL, FTS5,
no extension loading): by `build.ps1` into `build/obj/<target>/sqlite3.o`, by the
`Makefile` into `build/obj/sqlite3.o`.

To upgrade: download a newer amalgamation, verify its hash, replace both files,
update this README.
