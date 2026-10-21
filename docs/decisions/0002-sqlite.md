# 0002 - SQLite for the household's data

**Status:** accepted · **Since:** the start (2026-10)

## Context

The household's data - people, sign-ins, bookings, chores, notes, watch history, settings - is
small (thousands of rows) but precious, and it lives on an SD card that may lose power at any
moment. Hemma needs a store that never half-writes, needs no server of its own, and is easy to
back up.

## Decision

- SQLite, vendored (`third_party/sqlite`, pinned and hash-checked), in WAL mode.
- Tables are described in C (`FIELD`/`TABLE`) and kept in memory as typed records: reads cost
  nothing, every change is written through, an action runs in one transaction.
- A new field becomes a new column by itself; anything more is a numbered migration, and an
  older Hemma refuses data it can't read safely.
- DuckDB was considered and left out: C++, large, built for analytics rather than many small
  writes.

## Consequences

- One file holds everything (`hemma.db`); nightly backups are checked copies of it.
- A damaged file is rescued page by page with SQLite's recovery extension, and a rescue that gets
  next to nothing back refuses to start rather than begin a new household over the old one.
- SQLite's security fixes don't arrive by themselves: check its release history before a release
  ([SECURITY.md](../../SECURITY.md)).
