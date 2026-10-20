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

## Why C?

A home server is a small computer that stays on for years, often in a cupboard, with nobody
looking after it. Hemma is written in C, both the server and the code that draws its pages,
because C suits that job better than anything else:

- **Nothing sits between Hemma and the hardware.** There's no interpreter, no virtual machine
  and no garbage collector. A Node.js or Python app needs its runtime and tens of megabytes of
  memory before it does any work. Hemma uses **about 2.4 MB** on a Raspberry Pi, so even a
  Pi Zero 2 (512 MB) has room to spare.
- **One file that just runs.** C compiles to a single program for each kind of computer:
  Raspberry Pi, Linux, Windows and Mac. Installing Hemma means copying one file. Nothing to
  `npm install` or `pip install`, and no runtime update that can break it next year.
- **Almost no outside code.** The only library Hemma doesn't write itself is SQLite, which is
  also C. A big package tree (hundreds of packages from strangers) is one of the most common
  ways software gets broken or hacked. Hemma doesn't have one.
- **Fast where it counts.** Hemma converts videos itself: DVD rips become MP4s that every
  phone and TV can play. Its video decoders and encoders are written in C for Hemma, and a
  22-minute DVD episode converts in under five minutes on one PC core.
- **The same code runs in your browser.** C compiles to WebAssembly, so when you upload a
  video, *your computer* runs Hemma's converter and only the finished, smaller copy travels
  to the Pi. One codebase runs on the Pi, the PC and inside the browser.
- **Built to last.** C programs from decades ago still compile today, and there's a C compiler
  for practically every chip. A household server shouldn't need rewriting when a framework
  goes out of fashion.
- **Cheap to run.** When nobody is using it, Hemma wakes up about **0.2 times a second**.
  The Pi stays cool and quiet and draws only a few watts.

C asks for care in return: one careless line can crash the program or open a security hole.
Hemma pays that back with **over 500 automated tests** (about 1.6 million checks), debug
builds that stop at the first sign of undefined behaviour, the whole suite run again under
AddressSanitizer and UBSan on every push, a fuzzer that feeds the video converter damaged
files, HTML that is escaped by default, and several full code reviews so far.

## Building

| | Windows (`zig cc`) | Linux, macOS, Raspberry Pi (`make`) |
|---|---|---|
| Compiler | [zig](https://ziglang.org) (tested with 0.17.0): `winget install zig.zig` | `cc` (gcc or clang); `make CC="zig cc"` works too |
| Debug build | `.\build.ps1` (UBSan traps on) | `make -j8` (optimised; one object per file, so `-j` uses every core) |
| Run, dev mode | `.\build.ps1 run` → http://127.0.0.1:8080 | `make run` → http://127.0.0.1:8080 |
| Tests | `.\build.ps1 test [filter]` | `make test` (`make test T=lan` for a filter) |
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
| Linux x86-64 | Built with `make` and the full test suite run on every push (GitHub CI, Ubuntu), also under AddressSanitizer + UBSan |
| Linux ARM64, ARMv6/7 | Cross-compiled (static) without warnings |
| macOS | Built with `make` and the full test suite run on every push (GitHub CI); cross-compiled from Windows too |
| `make` with gcc on a Pi | Built and the full test suite run with gcc 14 on a Pi 3B+ (Debian 13) |

Platform-specific code lives in `core/plat.c`/`plat.h` (sockets, files, threads,
randomness) and `core/lan.c` (network interface, ARP/neighbour table), plus two clock
calls in `core/base.c`. Each has Windows, Linux and macOS paths. Keep `#ifdef`s out of
everything else.

## Tests

Tests sit next to the code as `*_test.c` and register themselves (`tests/test.h`). The
runner links all of them. `tests/harness.h` boots the whole core without a network
server, so module tests can sign people in, run actions and check the rendered pages.
There are over 500 tests, including end-to-end tests over real sockets. Keep the suite green.
GitHub CI runs them on Linux and macOS, again under AddressSanitizer + UBSan, and fuzzes the
video converter for 90 seconds (`tools/convfuzz.c`: a libFuzzer target; build it with
`-DCONVFUZZ_MAIN` for a replay/mutate driver that works with zig on Windows too).

```powershell
.\build.ps1 test            # everything, one process per core
.\build.ps1 test lan        # tests whose name or file contains "lan", in one process
```

The whole suite runs in one process per core: `build/test --shard K/N` runs every Nth test from
the Kth, and `make test` (`tests/parallel.sh`) and `.\build.ps1 test` start N of them and add up
the results (`HEMMA_TEST_JOBS` chooses N). Every process gets its own scratch folder, so a test
must not lean on what an earlier one left behind - running it in different company shows when it
does. On a 20-thread PC the suite takes about 5-10 s instead of a minute.

### Checking before you push

`tools/ci/local.sh` runs CI's three Linux jobs on your own machine: `test` (gcc, `make` +
`make test`), `asan` (the suite under AddressSanitizer + UBSan with clang) and `fuzz [seconds]`
(the converter's libFuzzer target). It works on a fresh copy of the tree in a RAM disk and keeps
each job's compiled objects for the next run, so only what changed compiles again: a run takes
seconds to a minute. It needs `make`, `gcc`, and for `asan`/`fuzz` clang with its sanitizer
runtimes (Ubuntu: `build-essential clang libclang-rt-18-dev`).

```sh
bash tools/ci/local.sh test            # Linux: this checkout (asan, fuzz [seconds] likewise)
```

On Windows it runs in WSL: `.\build.ps1 ci [test|asan|fuzz]` packs the checkout and hands it
over (`$env:HEMMA_WSL` picks the distribution). Setting WSL up, once (admin rights and a restart;
the distribution's disk can live on another drive):

```powershell
wsl --install --no-distribution                          # then restart
wsl --install Ubuntu-24.04 --location D:\WSL\Ubuntu      # asks for a Linux user name and password
wsl -d Ubuntu-24.04 -u root -- sh -c "apt-get update && apt-get install -y build-essential clang libclang-rt-18-dev"
```

A few tests look at real media when you point them at it (they're skipped otherwise, and
only read): `HEMMA_TEST_SHOW=<a show folder>` with the filter `media_real` prints how the
library reads that folder; `HEMMA_TEST_MKV=<file.mkv>`, `HEMMA_TEST_AC3=<file>` and
`HEMMA_TEST_CONVERT=<file.mkv>` run the demuxer, the AC-3 decoder and the converter (its
first 30 s) over a real file. `AAC_VERBOSE=1` / `AC3_VERBOSE=1` print codec details in
the codec tests.

## Versions and releases

Every pull request raises the version: the [VERSION](../VERSION) file at the top of the repo,
just `MAJOR.MINOR.PATCH` as [semver.org](https://semver.org) describes it. The patch number for
fixes only, the minor one for something new or changed, the major one for a big change in how
things work. The builds read it (into `core/version.c` alone, so a new version recompiles one
file). Each version gets its own section at the top of [CHANGELOG.md](../CHANGELOG.md), headed
with the version and the day, `## 0.14.0 (2026-10-19)`: that section is what the release says
changed, and a test fails until it's there.

**Merging releases it.** When main gets a version that has no release yet,
`.github/workflows/release.yml` runs the suite and builds the programs on Windows
(`.\build.ps1`, so every download has the in-browser converter), packs the downloads
(`tools/ci/bundle.sh`), tags the merged commit `vX.Y.Z` and publishes the release on the
[releases page](https://github.com/kloparn/Hemma/releases). Its notes are that version's
CHANGELOG section, then every commit since the previous release. Nothing to tag by hand; a
version without a CHANGELOG section stops it.

A Mac, Linux or Pi download is a `.tar.gz` (it keeps the program executable), and a Linux or Pi
one holds the installer, the service files and `hemma-upgrade` too. Every download has a
`SHA256SUMS` inside, and the release's own covers them all - each signed (below). There's no
Windows download yet: Windows Defender's machine
learning calls `hemma.exe` a trojan (a false positive), so it waits until the program is signed
or cleared with Microsoft.

To see what a release would hold without publishing anything, start the workflow by hand
(Actions → release → Run workflow): the downloads are kept as an artifact of that run. Or pack
them locally after `.\build.ps1 pi`, `pi32`, `linux` and `mac`, on Linux or in WSL:
`sh tools/ci/bundle.sh build <output folder>`.

### Signed releases

Installed Hemmas update only to what the project signed (issue #73): every `SHA256SUMS` in a
release has a `SHA256SUMS.sig` beside it, an Ed25519 signature (`core/ed25519.c`, ours, after
TweetNaCl) made with the project's secret key. The public half is built into the program
(`RELEASE_KEYS` in `core/release.c`), and `hemma-upgrade` asks the **installed** program,
`hemma --verify-release <folder>`, before it installs anything.

- **The key, once:** `hemma --gen-signing-key <file>` writes the secret (64 hex digits) into a new
  file only you can read and prints the public half. Keep the file offline, with a copy
  somewhere safe. The public half goes into `RELEASE_KEYS`; the file's contents into the GitHub
  secret `HEMMA_SIGNING_KEY` (Settings → Secrets and variables → Actions).
- **CI signs:** release.yml's publish job runs `tools/ci/bundle.sh` with `HEMMA_SIGNER` = the
  Linux program it just built; `hemma --sign` refuses a key whose public half isn't built in, and
  each signed folder is checked with `--verify-release` before it's packed. A release without the
  secret fails; a dry run without it warns and packs unsigned.
- **A new key:** add its public half beside the old one and release that version, signed with
  the old key. Once households have it, sign with the new one (and later drop the old one).
- **A lost or leaked secret:** release a version with a new key in place of the old one. A lost
  one can't sign it, so `hemma-upgrade` refuses it: households then install that version once by
  hand (the installer), as from 0.20.0.
- **By hand:** unpack a download and run `hemma --verify-release <its folder>` with a Hemma you
  trust (exit 0 = signed by the project).

## Project layout (short)

```
core/       the framework: platform layer, HTTP/WebSocket server, HTML, SQLite store,
            sign-in, the live UI (app.c), UI components (ui.c), command line (main.c)
modules/    one folder per feature: home book plan media tv board net people screens system,
            plus av (a pure-C video/audio codec library used by media)
web/        app.css, wire.js (the generic transport), conv.js (upload converter worker), icons, manifest
deploy/     hemma.service + install-pi.sh (systemd Linux), local.hemma.plist (macOS)
.github/    CI (build.yml: every push) and releases (release.yml: a version tag)
docs/       these guides
tests/      test framework, runner, harness
tools/      embed.c (bakes web/ into the binary), pisim.c (Pi-like CPU/memory limits on
            Windows), ci/ (CI's jobs locally, the release bundles), and dev tools for the codecs
third_party/sqlite/   SQLite amalgamation (public domain)
```

A module is a folder with a `MODULE(...)` declaration. Pages, actions, widgets, data
tables and CSS all come from there. The contract and an example are in
[CLAUDE.md](../CLAUDE.md#writing-a-module-the-contract--see-coreapph) and
`core/app.h`.

## Contributing

Hemma lives on GitHub: [kloparn/Hemma](https://github.com/kloparn/Hemma).

- **Found a bug, or have an idea?** Open an
  [issue](https://github.com/kloparn/Hemma/issues). For a bug, say what you did, what you
  expected and what happened instead, and the version (`hemma --version`, or the System
  page). The log from the System page (**Download the log**) helps a lot.
- **Found a security problem?** Please don't open a public issue: [SECURITY.md](../SECURITY.md)
  says how to report it privately.
- **Sending a change?** Open a pull request. Every pull request raises the version
  ([VERSION](../VERSION): the last number for fixes, the middle one for something new or
  changed) and adds its own section to [CHANGELOG.md](../CHANGELOG.md). The CI run
  must be green. Contributions are under the project's license, Apache 2.0
  ([LICENSE](../LICENSE)).

Ground rules:

- Everything is C. No JavaScript frameworks, npm or CDNs; app logic never goes in JS.
- Pages work without JavaScript too (plain links and forms).
- Zero compiler warnings (`-Wall -Wextra`). Every change comes with tests.
- Keep it light: the target is a Raspberry Pi, and the event loop must never block.
