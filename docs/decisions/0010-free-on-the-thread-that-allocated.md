# 0010 - Memory is freed by the thread that allocated it

**Status:** accepted · **Since:** 2026-10-28 (Hemma 1.5.1)

## Context

Hemma's event loop hands slow work to threads: every upload PUT (a copy kept in the browser's memory
goes up in 16 MB pieces, so a 1 GB episode is about 60 PUTs), every video request, subtitle picture,
download, the library scan. Each gets a small record the loop allocated (its job, the bytes
`conn_detach` handed over), and the thread used to `free()` it when done.

On the household's Raspberry Pi, Hemma held about 20 MB after a start and 100 MB after an afternoon
of uploads; a long upload added a megabyte or more a minute. Nothing was lost: LeakSanitizer found
nothing, and glibc's own count of memory in use stayed flat. The difference was the C library. The
Pi and Linux programs are static builds on **musl**, whose malloc doesn't reuse a block another
thread freed while the thread that allocated it kept allocating - and the event loop never stops.
A 70-line program with no Hemma code in it shows it (20,000 hand-offs of a 2.2 KB block): musl, the
other thread frees it - 60 KB to 8 MB and climbing; musl, handed back and freed by the allocating
thread - flat; glibc - flat. Sizes from 16 bytes to 16 KB were affected; the other direction (a
thread allocates, the event loop frees) was not.

## Decision

- **What the event loop allocated, the event loop frees.** A worker thread hands such memory back
  with `free_on_loop()` / `buf_free_on_loop()` (core/base.h); the loop frees it on its next turn
  (`free_on_loop_now()` in `server_run`, and in `app_halt`). What a worker allocated itself it frees
  itself - and the loop may free what a worker made.
- A worker never **grows** a buffer the loop gave it (realloc frees the old bytes on the worker): it
  copies it into one of its own.
- `thread_spawn`'s own start record follows the same rule.

## Consequences

- Every worker in Hemma was changed (upload, stream, zip, subtitle pictures, backup downloads,
  uploaded updates, the library scan, subtitle extraction, lookups). A new worker follows the rule;
  CLAUDE.md lists it with the module rules.
- Handing back allocates a small node on the worker (the direction that is fine), so any block can be
  handed back, whatever its size.
- Windows and glibc builds never showed the problem and lose nothing by the rule. A memory check
  belongs on a musl build: the Pi's, or `x86_64-linux-musl` run in WSL.
