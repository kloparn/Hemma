# SQLite (vendored)

- Version: 3.54.1, unmodified
- License: public domain — https://www.sqlite.org/copyright.html

**The amalgamation** (`sqlite3.c`, `sqlite3.h`):

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the zip: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
  (matches the hash published on https://www.sqlite.org/download.html)
- SHA-256 of the files: `sqlite3.c` `0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52`,
  `sqlite3.h` `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c`

**The recovery extension** (`dbdata.c`, `sqlite3recover.c`, `sqlite3recover.h` - what the
`sqlite3` shell's `.recover` uses; Hemma rescues a damaged database with it):

- Source: `ext/recover/` in https://www.sqlite.org/2026/sqlite-src-3540100.zip
- SHA3-256 of the zip: `877c6f4996ce7adb368686bce21638de9e3d91534832ad3d9d119037d1bc22ad`
  (matches the hash published on https://www.sqlite.org/download.html)
- SHA-256 of the files: `dbdata.c` `0a57eae22b51507c28f6c647fba00ccacd39b5aaf3393dfe9435fb04810a6e0e`,
  `sqlite3recover.c` `792ba92b381921098fdc637b2f1e97dee06f505b06d29c832b48160ed58c4339`,
  `sqlite3recover.h` `c088088d15af055304e931842f7c197024d46ff1b7077912ec42149e9fa4f9bb`
- `recover.c` is Hemma's: it includes those two `.c` files so they build as one object.
  The extension needs `sqlite3.c` built with `SQLITE_ENABLE_DBPAGE_VTAB`.

Built once per target, with the compile options listed in the build files (WAL, FTS5,
the page reader for recovery, no extension loading): by `build.ps1` into
`build/obj/<target>/sqlite3.o` and `recover.o`, by the `Makefile` into `build/obj/`.

To upgrade: download a newer amalgamation and the matching source zip, verify their hashes,
replace the five files (`ext/recover/` holds the extension's three), update this README.
