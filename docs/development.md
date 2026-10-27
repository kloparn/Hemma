# Developing Hemma

Hemma is a household hub written in C11. It has no dependencies besides the SQLite
amalgamation in `third_party/sqlite/` (and, in `third_party/ffmpeg/`, two tables of numbers its DTS
decoder takes from FFmpeg: the DTS specification leaves them out). The server renders every page as HTML and pushes
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
  also C (the one other borrowing is data: two DTS tables from FFmpeg). A big package tree (hundreds of packages from strangers) is one of the most common
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
  The Pi stays cool and quiet and draws only a few watts. **Eight people streaming at once**
  (8 Mbit/s each) used about **10% of one core** on a simulated Pi 3, with memory peaking at
  8.6 MB; on a real Pi 3B+ eight such streams ran with zero errors. Uploading a 3.6 GB season
  ran at full disk speed in 9 MB of memory.

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
`-DCONVFUZZ_MAIN` for a replay/mutate driver that works with zig on Windows too). Every night it
fuzzes for about three hours (see [Fuzzing](#fuzzing)), and on every push it also opens Hemma in
a real browser (see [The browser's side](#the-browsers-side)).

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
`make test`), `asan` (the suite under AddressSanitizer + UBSan with clang) and `fuzz [seconds]
[conv|mkv|ac3|mpeg2]` (a libFuzzer target - see [Fuzzing](#fuzzing)). The suite runs in four
processes there, as on GitHub's runners (`HEMMA_TEST_JOBS` changes it): tests that share a process
meet what earlier ones left, and four processes put different tests together than twenty do.
It works on a fresh copy of the tree in a RAM disk and keeps
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
`HEMMA_TEST_CONVERT=<file.mkv>` run the demuxer, the AC-3 / E-AC-3 decoder and the converter
(its first 30 s; it saves the file's subtitles next to it, so point it at a copy) over a real
file; `HEMMA_TEST_DTS=<file.mkv>` (filter `dts_real`) decodes a DTS track and checks every frame,
and with `HEMMA_TEST_DTS_WAV=<file.wav>` writes every channel as floats, to compare with another
decoder. `AAC_VERBOSE=1` / `AC3_VERBOSE=1` print codec details in the codec tests.
`tools/ac3dump.c` (build line at its top) decodes AC-3 and E-AC-3 from an MKV or a raw stream:
`stats` reports which coding tools a stream uses and whether every frame parsed, and `f32 ...
--native` writes every channel as raw floats in WAV order - what `ffmpeg -f f32le` writes, for
comparing with another decoder (expect dither and noise to differ, nothing else).

### The browser's side

`web/wire.js` is more than a transport: it holds Hemma's video player (resume, progress, the next
episode, Live TV's "now") and the upload client. `tests/browser` checks it where it runs - in
Google Chrome, driven by [Playwright](https://playwright.dev), against a real Hemma with real
videos - and lints `web/*.js` with eslint. It's Node, for CI and your machine only: nothing in it
ships, and the program never needs it.

```sh
make && make build/test                  # or on Windows: .\build.ps1 build; .\build.ps1 test av_test
cd tests/browser && npm ci
npx playwright test                      # the pages in Chrome (needs Google Chrome installed)
npm run lint                             # eslint over web/*.js
```

`server.mjs` makes everything afresh each run: a folder under your temp folder, small test videos
made by Hemma's own encoders (`build/test av_test_clips` with `HEMMA_TEST_CLIPS`), an admin, and
Hemma itself on `127.0.0.1:18093`. A test that changes the player's behaviour on purpose changes
its spec too; the rules are listed in CLAUDE.md ("the player's rules").

### Fuzzing

The converter reads files nobody controls, in the process that serves the whole household, so it's
fuzzed: libFuzzer feeds it damaged and strange inputs under AddressSanitizer + UBSan. Seven targets
(`tools/ci/fuzz.sh TARGET [seconds] [processes]`): `conv` - a whole conversion
(`tools/convfuzz.c`, slow per input, reaches everything) - and `mkv`, `ac3`, `dts`, `mpeg2`, `pgs` and
`vobsub` (Blu-ray and DVD picture subtitles), one part alone (`tools/avfuzz.c`, tens to hundreds of
times as many inputs a minute). Every push runs `conv` for 90 s; every night
`.github/workflows/fuzz.yml` runs all seven side by side for about three
hours, each starting from what the earlier nights found. A finding fails that night's run, and the
input that does it is kept with the run: `build/fuzz-<target> <file>` replays it.

```sh
bash tools/ci/local.sh fuzz 300 ac3      # five minutes of the Dolby decoder (Windows: .\build.ps1 ci fuzz 300 ac3)
```

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

### The Raspberry Pi image

Every release also has `hemma-<version>-pi.img.xz` (issue #71): the official Raspberry Pi OS Lite
(64-bit) with Hemma's Pi download and a first-start unit added. `tools/image/build.sh` makes it
**without root**: it never mounts or runs anything from the image, it writes into the image's ext4
partition with `debugfs`, so it builds on any Linux (CI's runner, WSL):

```sh
sh tools/ci/bundle.sh build dist            # the downloads, among them hemma-pi64.tar.gz
HEMMA_IMAGE_CACHE=~/img-cache sh tools/image/build.sh dist/hemma-pi64.tar.gz dist
```

It checks the base image's SHA-256 (Raspberry Pi's, beside it), and fetches the few packages the
image lacks for security updates (`unattended-upgrades`, `acl` and their dependencies -
`tools/image/debs.awk` works out which) from Debian, checked against Debian's signed index with the
keyring inside the image itself. At the first start, `hemma-firstboot.service` installs those,
names the machine `hemma` unless Raspberry Pi Imager named it, and runs the bundled
`install-pi.sh`: the password secret and the setup code are made on that Pi. Nothing secret is in
the image (no users, no SSH host keys, `/etc/machine-id` unset). In CI, `HEMMA_IMAGE=1` makes
bundle.sh build it, and the release's signed `SHA256SUMS` covers it. It adds a few minutes to a
release (a 550 MB download, unpacking, packing 3 GB again with xz).

**Only the newest release keeps its image:** once a release is out, release.yml deletes the
`.img.xz` from every older one (only when the new one has its image). Their program downloads stay,
and so do their `SHA256SUMS` - `hemma-upgrade` checks only the files it takes.

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
third_party/ffmpeg/   two DTS vector tables from FFmpeg (LGPL 2.1+; see its README)
```

A module is a folder with a `MODULE(...)` declaration. Pages, actions, widgets, data
tables and CSS all come from there. The contract and an example are in
[CLAUDE.md](../CLAUDE.md#writing-a-module-the-contract--see-coreapph) and
`core/app.h`.

## Contributing

[CONTRIBUTING.md](../CONTRIBUTING.md) says how to report a problem, how to send a change and what a
change needs before it's merged; [ARCHITECTURE.md](../ARCHITECTURE.md) is the map of the program, and
[docs/decisions](decisions/README.md) holds the reasons behind its big choices. A security problem goes
privately: [SECURITY.md](../SECURITY.md).
