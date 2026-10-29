# Decisions

Why Hemma is built the way it is: one short record per big choice, in the order they were made.
Each says what was decided, why, and what it costs. A decision can change - then a new record
replaces the old one, which stays here marked as such.

| # | Decision | Since |
|---|---|---|
| [0001](0001-c-and-server-drawn-pages.md) | Everything in C; pages drawn on the server | 2026-10 |
| [0002](0002-sqlite.md) | SQLite for the household's data | 2026-10 |
| [0003](0003-own-video-codecs.md) | Our own video codecs, not ffmpeg | 2026-10 |
| [0004](0004-plain-http-at-home.md) | Plain http, inside the home network | 2026-10-11 |
| [0005](0005-nothing-leaves-home.md) | Nothing leaves the home unasked | 2026-10 |
| [0006](0006-formats-grow-with-fuzzing.md) | New video formats when real files need them, each fuzzed | 2026-10-21 |
| [0007](0007-reviews-and-releases.md) | A written review for the core; merging releases | 2026-10-21 |
| [0008](0008-three-languages-built-in.md) | English, Swedish and Spanish, built in | 2026-10-20 |
| [0009](0009-categories-looked-up-when-asked.md) | Categories may be looked up online, when an admin asks (amends 0005) | 2026-10-27 |
| [0010](0010-free-on-the-thread-that-allocated.md) | Memory is freed by the thread that allocated it | 2026-10-28 |
| [0011](0011-an-android-app-as-a-thin-window.md) | An Android app: a thin window onto Hemma, served by Hemma itself | 2026-10-28 |

New record: copy the shape of one of these (status, context, decision, consequences), number it
next, add it to the table, and link it from the pull request that acts on it.
