# 0001 - Everything in C; pages drawn on the server

**Status:** accepted · **Since:** the start (2026-10)

## Context

Hemma runs all day on a small computer in a cupboard - a Raspberry Pi 3 is enough - and serves
every phone, laptop, TV and tablet in the home. It should be small, fast, and still be running
in ten years without a pile of dependencies to keep up with. Its pages need to update live on
every screen at once (someone books the car; the hallway tablet shows it).

## Decision

- The whole program is C11: the server, the pages, the database layer, the video codecs. The
  only third-party code is SQLite (vendored).
- The server draws every page as HTML. Changes reach the browser as "morphs": the server
  draws the page again, and a small script (`web/wire.js`) patches the open page to match.
- No JavaScript frameworks, npm packages or CDNs in what ships. Pages work without JavaScript
  too: links and forms underneath.
- Two things can only happen in a browser, so they live in wire.js: the video player (its
  controls, resuming, Live TV) and the upload client. The rules the household relies on are
  kept by the server wherever possible, listed, and tested in a real browser (issue #49).
- Uploads convert videos on the sending device with Hemma's own C converter compiled to
  WebAssembly - still C.

## Consequences

- A static binary of about 3 MB, a few MB of memory, one process to install.
- No build tool chain beyond a C compiler (`zig cc` cross-compiles everything from Windows).
- Writing HTML in C takes care: `H()` escapes by default, and every page is covered by tests
  that render it.
- The player and the upload client are real JavaScript and need their own checks: eslint and
  Playwright run in CI (Node is fine there - it never ships).
- More in [Why C?](../development.md#why-c).
