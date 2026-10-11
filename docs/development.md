# Developing Hemma

Hemma is a household hub written in C11. It has no dependencies besides the SQLite
amalgamation in `third_party/sqlite/`. The server renders every page as HTML and pushes
changes to open screens over a WebSocket. The browser runs one small, generic script
(`web/wire.js`) and no app logic. Uploads also use a worker (`web/conv.js`) that runs
the C video converter compiled to WebAssembly. Features are self-contained modules under
`modules/<id>/` that register themselves, so adding one touches no shared file.

**[CLAUDE.md](../CLAUDE.md) is the working contract.** It covers the architecture, the
module API, conventions, gotchas and the current status. Read it before changing code.
This page is the short version for getting started.

## Building

| | Windows (`zig cc`) | Linux, macOS, Raspberry Pi (`make`) |
|---|---|---|
| Compiler | [zig](https://ziglang.org) (tested with 0.17.0): `winget install zig.zig` | `cc` (gcc or clang); `make CC="zig cc"` works too |
| Debug build | `.\build.ps1` (UBSan traps on) | `make` (optimised) |
| Run, dev mode | `.\build.ps1 run` → http://127.0.0.1:8080 | `make run` → http://127.0.0.1:8080 |
| Tests | `.\build.ps1 test [filter]` | `make test` |
| Release | `.\build.ps1 release` | `make` |
| Install | (see [windows.md](windows.md)) | `sudo make install` (systemd Linux) |

Dev mode (`--web web`) serves `web/` and the module CSS from disk. Editing them reloads
open browsers.

**Cross-compiling** (Windows, with zig) produces static, dependency-free binaries:

| Command | Output | Target |
|---|---|---|
| `.\build.ps1 pi` | `build/hemma-pi64` | aarch64 Linux (musl, static): Raspberry Pi 64-bit, any ARM64 Linux |
| `.\build.ps1 pi32` | `build/hemma-pi32` | armv6 hard-float Linux (musl, static): every Pi, any 32-bit ARM Linux |
| `.\build.ps1 linux` | `build/hemma-linux` | x86-64 Linux (musl, static) |
| `.\build.ps1 mac` | `build/hemma-mac-arm64`, `build/hemma-mac-x64` | macOS 11+ (links only libSystem; the arm64 file is ad-hoc signed by zig) |
| `.\build.ps1 all` | all of the above | after running the tests |

Build switches: `$env:HEMMA_SKIP_MODULES="media,tv"` leaves module folders out (`make
SKIP="media tv"`); `$env:HEMMA_BUILD_DIR="build-x"` builds into a private folder. The
browser-side video converter (`av.wasm`) is built when zig is available. Without zig,
`make` builds Hemma without it.

### Platform status

| Platform | How it's checked |
|---|---|
| Windows x86-64 | The development platform: built and the full test suite run here |
| Raspberry Pi OS 64-bit (aarch64) | Cross-compiled; runs in daily use on a Pi 3B+ |
| Linux x86-64, ARM64, ARMv6/7 | Cross-compiled (static) without warnings |
| macOS arm64 / x86-64 | Cross-compiled without warnings (server and test suite); not yet run on a real Mac |
| `make` with gcc on Linux/Pi, `make` on macOS | Expected to work; not yet confirmed on every system |

Platform-specific code lives in `core/plat.c`/`plat.h` (sockets, files, threads,
randomness) and `core/lan.c` (network interface, ARP/neighbour table), plus two clock
calls in `core/base.c`. Each has Windows, Linux and macOS paths. Keep `#ifdef`s out of
everything else.

## Tests

Tests sit next to the code as `*_test.c` and register themselves (`tests/test.h`). The
runner links all of them. `tests/harness.h` boots the whole core without a network
server, so module tests can sign people in, run actions and check the rendered pages.
There are over 350 tests, including end-to-end tests over real sockets. Keep the suite green.

```powershell
.\build.ps1 test            # everything
.\build.ps1 test lan        # tests whose name or file contains "lan"
```

A few tests look at real media when you point them at it (they're skipped otherwise, and
only read): `HEMMA_TEST_SHOW=<a show folder>` with the filter `media_real` prints how the
library reads that folder; `HEMMA_TEST_MKV=<file.mkv>`, `HEMMA_TEST_AC3=<file>` and
`HEMMA_TEST_CONVERT=<file.mkv>` run the demuxer, the AC-3 decoder and the converter (its
first 30 s) over a real file. `AAC_VERBOSE=1` / `AC3_VERBOSE=1` print codec details in
the codec tests.

## Project layout (short)

```
core/       the framework: platform layer, HTTP/WebSocket server, HTML, SQLite store,
            sign-in, the live UI (app.c), UI components (ui.c), command line (main.c)
modules/    one folder per feature: home book plan media tv board net people wall,
            plus av (a pure-C video/audio codec library used by media)
web/        app.css, wire.js (the generic transport), conv.js (upload converter worker), icons, manifest
deploy/     hemma.service + install-pi.sh (systemd Linux), local.hemma.plist (macOS)
docs/       these guides
tests/      test framework, runner, harness
tools/      embed.c (bakes web/ into the binary), pisim.c (Pi-like CPU/memory limits on
            Windows), and dev tools for the codecs
third_party/sqlite/   SQLite amalgamation (public domain)
```

A module is a folder with a `MODULE(...)` declaration. Pages, actions, widgets, data
tables and CSS all come from there. The contract and an example are in
[CLAUDE.md](../CLAUDE.md#writing-a-module-the-contract--see-coreapph) and
`core/app.h`.

## Contributing

> **TODO (owner):** how to report bugs, propose changes and send patches once the
> project is published (issues, pull requests, code of conduct).

Ground rules that will stay the same:

- Everything is C. No JavaScript frameworks, npm or CDNs; app logic never goes in JS.
- Pages work without JavaScript too (plain links and forms).
- Zero compiler warnings (`-Wall -Wextra`). Every change comes with tests.
- Keep it light: the target is a Raspberry Pi, and the event loop must never block.
