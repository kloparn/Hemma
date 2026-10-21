# Contributing to Hemma

Thanks for wanting to help. Hemma is a household's own page on its home network, written in C
from top to bottom. This page says how to report a problem, how to send a change and what a
change needs before it's merged. How the program fits together is in
[ARCHITECTURE.md](ARCHITECTURE.md); why it's built that way is in [docs/decisions](docs/decisions/README.md).

By taking part you agree to the [code of conduct](CODE_OF_CONDUCT.md).

## Reporting a problem

[Open an issue](https://github.com/kloparn/Hemma/issues/new/choose) and pick **Bug report**. The
most useful things to include:

- the version: `hemma --version`, or System -> **Help** -> "Asking for help";
- what you did, what you expected, and what happened instead;
- which device and browser (a TV's browser behaves differently from a phone's);
- the log: **Download the log** on that same card. It holds no passwords, but it does name people
  and devices in your home - have a look before you attach it.

**A security problem?** Don't open a public issue: [SECURITY.md](SECURITY.md) says how to report
it privately.

**An idea?** Pick **Feature request**. Hemma is for households, runs on a small computer, and
keeps everything at home ([0005](docs/decisions/0005-nothing-leaves-home.md)): ideas that fit
those go furthest.

## Setting up

[docs/development.md](docs/development.md) has the details. In short:

```sh
make -j8 && make test                    # Linux and macOS (any C compiler)
```

```powershell
.\build.ps1 test                          # Windows, with zig (winget install zig.zig)
.\build.ps1 run                           # http://127.0.0.1:8080, pages reload as you edit web/
```

Before you push, run CI's Linux jobs on your machine - the suite with gcc, again under
AddressSanitizer + UBSan, and a short fuzz run - in about a minute: `bash tools/ci/local.sh test`
(Windows: `.\build.ps1 ci test` in WSL). If your change touches the video player or uploads, also
run the browser tests (`tests/browser`: [how](docs/development.md#the-browsers-side)).

## What a change needs

- **C, drawn on the server.** Everything is C11; pages are HTML drawn by the server and morphed
  into the browser. No JavaScript frameworks, npm packages or CDNs in what ships. Pages work
  without JavaScript too. `web/wire.js` holds only what must run in a browser (the player, the
  upload client) - see [0001](docs/decisions/0001-c-and-server-drawn-pages.md).
- **Tests.** Every change comes with tests: `*_test.c` next to the code, auto-registered. Module
  tests boot the whole core in memory (`tests/harness.h`): sign people in, run actions, check the
  pages. The suite stays green.
- **Zero warnings** with `-Wall -Wextra`, on every target (`.\build.ps1 all` cross-compiles them).
- **Light.** The target is a Raspberry Pi 3. The event loop never waits: disk scans, streams and
  conversions go to threads, and their results come back on the next tick.
- **Words for people.** Hemma's users are a household, children included: plain, friendly words,
  phone first. Check new pages at phone width (390 px): nothing should scroll sideways.
- **Three languages.** Every text people read goes through `T("English")` (or `N_()` for text shown
  later), with its Swedish and Spanish in the module's `*_lang.c` table - the tests fail without
  them ([0008](docs/decisions/0008-three-languages-built-in.md)). Don't speak Swedish or Spanish?
  Say so in the pull request, and someone will help.
- **A new video format** comes with its fuzz seed and tests ([0006](docs/decisions/0006-formats-grow-with-fuzzing.md)).

### Style

- C11 (`-std=gnu11`), four spaces, `snake_case`, fixed-size strings copied with `str_copy`.
- HTML only through `H()`, which escapes by default (`%r` for trusted markup).
- Platform code only in `core/plat.c` and `core/lan.c` (Windows, Linux, macOS).
- Comments say why, in plain sentences. Match the code around you.
- A new feature is a module: its own folder under `modules/`, its CSS classes prefixed with its id.
  [CLAUDE.md](CLAUDE.md) ("Writing a module") is the full contract, rule by rule.

## Sending a pull request

- **One thing at a time.** Aim for under about 1,500 changed lines (translations and test data
  don't count); keep features and refactors in separate pull requests.
- **The version and the changelog.** Every pull request raises the version in [VERSION](VERSION) -
  the last number for fixes, the middle one for something new or changed - and adds its own
  section at the top of [CHANGELOG.md](CHANGELOG.md), written for the household: what changed for
  them. **Merging publishes a release** by itself (signed downloads, the Raspberry Pi image).
- **The checklist** in the pull request template: tests, warnings, phone width, languages.
- **A written review for `core/`.** Changes to `core/` (the server, sign-in, the store, the app
  core) get a review written on the pull request before they're merged; an AI review counts if
  its findings are posted there and answered ([0007](docs/decisions/0007-reviews-and-releases.md)).
- **Green CI**: the suite on Linux and macOS, ASan + UBSan, the fuzzer, the browser tests, eslint.

## Licence

Hemma is under the [Apache License 2.0](LICENSE). By sending a change you agree that it's
under that licence too.
