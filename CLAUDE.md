# Hemma — household LAN hub in pure C

Hemma ("at home") is a self-hosted page for our home network: book shared things
(car, laundry, guest room…), plan (chores, shopping list, calendar), a notice board,
network/presence view, a media portal (movies/series from a local drive) and
"90's TV channels" that run continuously on a shared clock. README.md is the
owner-facing overview; this file is the working contract.

Runs on a Raspberry Pi in production; developed on Windows 11 with `zig cc`.

## Owner preferences (keep honouring these)

- **Everything in C** — backend *and* frontend. No JS frameworks, no npm, no CDNs.
  The browser runs `web/wire.js`: a small, *generic* transport (WebSocket, DOM
  morphing, form/link interception). App logic never goes in JS. The one exception
  in kind: uploads run Hemma's C converter compiled to WebAssembly (`av.wasm`, fed by
  the tiny Worker `web/conv.js`) - still C, just running on the sending device.
- **Server-driven UI**: C renders HTML; changes are pushed as DOM morphs over
  WebSocket to every open screen. Pages also work without JS (links + POST forms).
- **Fast and light**: single-threaded event loop, small static binary for the Pi.
  The only third-party code is SQLite (vendored, public domain).
- **Modular**: features are self-contained modules so several workers can build
  in parallel without touching each other's files.
- **Tests**: co-located `*_test.c`, auto-registered. Cover parsers, protocol code,
  module logic and rendered pages (via the harness). Keep the suite green.
- **Local database**: SQLite (`data/hemma.db`), chosen over DuckDB (C++, huge,
  analytics-oriented). People/accounts, preferences and module data live there.
- **Login in front of everything**: only the sign-in (and first-run welcome) page is public. The
  first admin is created on the welcome page (or, headless, from the settings file); the admin
  creates everyone else.
- Household audience: friendly, phone-first UI; English UI text, Swedish-aware input.
- **This file is shareable** (it goes in the repo): nothing machine-specific here - no paths on a
  dev PC, addresses, MACs, hostnames of anyone's machines, credentials or deployment diaries. The
  maintainer keeps those in private local notes (outside the repo).
- Keep this file updated as things change (owner's request).

## Layout

```
core/            framework with a stable API for modules
  base.*         buffers (always NUL-terminated), strings, UTF-8, time/calendar, hashing, logging
  plat.*         the ONLY place with #ifdef _WIN32 besides lan.c and base.c's two clock calls (sockets,
                 files, dirs, threads, random, env)
  lan.*          LAN facts: interface/gateway, ARP neighbours, probes, Wake-on-LAN, reverse-DNS packets
                 (Windows: iphlpapi; Linux: /proc + /sys; macOS: routing-table sysctl, parsed by the
                 portable lan_parse_rt_addrs so it's unit-tested on every platform)
  codec.*        SHA-1, SHA-256, HMAC, PBKDF2, Base64, hex, URL encoding
  json.* http.* ws.* server.*   protocol stack + event loop
  html.*         H() — HTML formatter that escapes by default (%s escaped, %r raw, %02d padding ok)
  store.*, db.h  typed tables kept in RAM, persisted in SQLite; db.h = raw SQL (+FTS5)
  when.*         natural-language dates/times (English + Swedish) for quick-add bars
  env.*          .env loading (real environment wins)
  auth.*         passwords (PBKDF2 + server salt), throttling, admin bootstrap, welcome/sign-in/forgot pages,
                 /login /logout /setup /forgot, recovery codes, --reset-password (see "Accounts")
  app.*          the live core: modules, routing, sessions, render→diff→morph, actions, presence,
                 settings, per-person prefs, activity feed, shared agenda, JSON API (/api)
  ui.*           components: shell, icons, avatars, cards, forms, modals, time formatting
  main.c         command line (--gen-salt, --env, --web ...)
modules/<id>/    one folder per feature; folder name == module id (home book plan media tv board net people wall)
                 dependencies: tv needs media (it links modules/media/library.c), media needs av (the converter)
  media/upload.c admins add videos from a browser (plan + streamed PUTs, .part files, resume); a folder
                 holding x.mkv + x.web.mp4 sends only the copy (checkbox "also send originals"). Other MKVs
                 (box "make videos play everywhere on this device first", on by default) are converted BY THE
                 UPLOADING BROWSER (todo {i,f}; web/conv.js runs av.wasm in Workers) and only x.web.mp4 is
                 sent: POST <batch>/<i>?pm= (progress) / ?size= (copy ready -> PUT) / ?original=1&why= (the
                 converter refused it: the original goes under its own name). The box (and data-convert/
                 data-wasm on the form) only appears when av.wasm is built in. Resume trusts a ".part" only
                 if it's no longer than the file and not a picture (resume_point); one file is in one
                 upload at a time (claimed); Stop stops the upload whose card it's on. ONE UPLOAD AT A
                 TIME (owner's rule): while one is running (workers, or its browser active in the last
                 30 s - Batch.busy), a new plan is refused with who/how far, unless "force" ("Upload
                 anyway", shown only then) - both run, the running one's files skipped. ALREADY HAVE IT: a video
                 the library has under another name (library.c media_have: named as it will be there; same
                 show+season+every episode of its range, or same film title+year+part via names.c
                 media_name_part "CD2") is skipped in the plan, before any converting, and so are its
                 subtitles/pictures (skip_companions); box "dupes" sends them anyway. Disc-rip titles numbered
                 only by order, extras and the file's own original/copy are never matched. A disc's
                 "play all" (spotted by size) sends just its first MB as x.playall.mkv: the header's duration
                 is what lets library.c tell featurettes from episodes without the play-all. The same
                 plan/PUT path sets pictures: {"art":"<series|film id>"} + one JPEG/PNG/WebP (magic-checked,
                 25 MB max) saved as <show folder>/poster.<ext> or <film folder>/<film name>.<ext> - where
                 the scan looks first (library.c media_art_target/_current/_prune); the page dialog is ?pic=1.
  media/convert.c "play everywhere": MKV -> <name>.web.mp4 next to the original (H.264 + AAC, faststart),
                 by av/conv.c (the same engine browsers run): MPEG-2 decoded, inverse-telecined/deinterlaced
                 (av/deint, chosen by sampling 4 windows) and re-encoded; H.264 copied; HEVC copied as 'hvc1'
                 (hvcC arrays marked complete - what Safari needs; no HEVC decoder here, so it plays on Apple
                 devices and browsers with HEVC hardware, not everywhere; Convert says "play in Safari too", the watch page
                 badge "Plays on newer devices");
                 AC-3 -> AAC; AAC copied. convert.c = files + the queue: one file per worker thread
                 (cores-2, max 12, background priority); the library pairs x.web.mp4 with x.* (MediaFile.web,
                 role MF_WEBCOPY hides the copy) and /media/file/<id> streams the copy (media_playable()).
                 Progress: copying reads the whole file once for timestamps first (COPY_SURVEY_PM = the first 30%
                 of the bar - minutes for a film on a Pi's SD card), then writes.
                 TWO VERSIONS (owner's rule: the one that plays in fewer places goes, the more compatible stays):
                 remove.c media_lesser_version() pairs x.* with x.web.mp4 when both last as long (2 s / 1%),
                 ranks them with media_compat_cost() (playable severity, then box/HEVC/audio; a tie keeps the
                 copy). Library settings: "Keep only the most compatible version" (media.drop_originals) and
                 "Do that by itself after each conversion" (setting media.drop_originals, off by default;
                 convert.c settle_finished -> convert_replace_original checks the new copy file itself).
                 Deleting an original moves everyone's progress to the copy (library.c media_watch_move).
                 Subtitles: x.en.srt belongs to x.mkv while it exists, else to x.web.mp4 (media_sub_for knows .web.mp4).
  media/remove.c admins delete a film or other video (watch page "Remove" -> ?remove=1 dialog listing every
                 file -> media.remove): the video, its copy, subtitles, picture - or its OWN folder as a whole
                 (folder name parses to the film's title/year, nothing inside but the film and extras - the
                 library never indexes extras, so belongs() asks the name parser; a link (DirEntry.is_link),
                 >400 entries or >4 levels deep keeps the folder). Empty parents go, never a top-level folder
                 ("Movies"). Also drops watch progress, logs to the activity feed. Episodes: not here.
  av/            pure-C codec library, no MODULE(), libc only (no core/ deps, no globals) so it also
                 builds for wasm32: mkv.* (Matroska demuxer: cues/bisect seeking, lacing, unknown
                 sizes), mpeg2.* (MPEG-2 MP@ML + MPEG-1 video decoder, display-order frames, IEEE 1180
                 IDCT), ac3.* (full A/52 decoder: native/WAV-order/Lo-Ro stereo float output, 1536
                 samples per frame; E-AC-3 unsupported). Feed mpeg2 the track's CodecPrivate first, then
                 each block with its pts; mpeg2_reset()/ac3_reset() after mkv_seek(). h264enc.* (Constrained
                 Baseline encoder, bit-exact vs Chrome; annexb=false + h264enc_avcc() for MP4; ~180 fps SD
                 per PC core, ~7.6 fps per Pi 3 core). mp4.* (faststart via reserved moov, or fragmented), flac.*, deint.* (IVTC / yadif / bob /
                 pass + analysis), aac.* (AAC-LC encoder, 48 kHz stereo 160 kbit/s for the converter;
                 aac_testdec.h = a small decoder for tests). conv.* = the converter (MkvReader in, Mp4Out
                 out, progress callback, `unsupported` = a codec it doesn't handle); used by media/convert.c
                 and by wasm/avconv.c, which build.ps1/Makefile compile to $B/av.wasm (zig cc -target
                 wasm32-wasi -mexec-model=reactor -msimd128; ~330 KB, imports only 4 hemma.* functions,
                 embedded as /static/av.wasm). Measured on a DVD episode: wasm ~75% of native speed (374 s
                 vs 282 s on one core), video bit-identical to native, AAC differs only by float rounding.
web/             app.css (core styles + shared vocabulary), wire.js, conv.js (the upload converter's Worker:
                 feeds av.wasm a File, gathers the copy), icon.svg, manifest
third_party/sqlite/  SQLite 3.54.0 amalgamation (see its README for provenance + hash)
tools/embed.c    bakes web assets (+ every module's CSS, bundled) into the binary
tools/pisim.c    Windows dev aid: runs a program inside Raspberry Pi-like limits (see below)
tools/avdump.c   dev aid for modules/av: info | frames --at S --n K --png DIR | bench | fuzz
                 (zig cc -O2 -std=gnu11 -Icore tools/avdump.c modules/av/mkv.c modules/av/mpeg2.c
                  core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt -o avdump.exe)
tools/ac3dump.c  info | stats | wav | raw | bench for AC-3 in MKV or raw .ac3
                 (zig cc -O2 -Imodules/av tools/ac3dump.c modules/av/ac3.c -o build/ac3dump.exe)
tools/aaccheck.c  <file.mkv | synth:RATE> <outdir> [--from S] [--seconds S] [--kbps K]: AC-3 -> AAC + WAV
                 (zig cc -O2 -std=gnu11 -Icore tools/aaccheck.c modules/av/aac.c modules/av/ac3.c modules/av/mkv.c
                  core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt)
tools/muxcheck.c MKV -> MP4 remux, FLAC variant, deinterlace PNGs (build line at the top of the file)
tools/h264check.c encode | synth | bench: MKV -> H.264 Annex-B + reconstruction PNGs, PSNR (the
                 encoder was once checked bit-exact against Chrome's WebCodecs decoder)
                 (zig cc -O2 -std=gnu11 -Icore tools/h264check.c modules/av/h264enc.c modules/av/mkv.c
                  modules/av/mpeg2.c core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt)
tests/           test.h (framework), runner.c, harness.* (boot the core without a server)
deploy/          hemma.service (hardened systemd unit), install-pi.sh (any systemd Linux, despite the name),
                 local.hemma.plist (macOS launchd agent)
docs/            user guides: raspberry-pi linux windows macos home-network ssh accounts development
                 (README.md links them). Public docs: generic examples only (192.168.1.50, hemma.local,
                 /srv/media) - never this household's names, addresses, paths or passwords
build*/ data/ .env   generated / runtime / secrets (gitignored)
```

## Build, run, test

```powershell
./build.ps1              # debug build -> build/hemma.exe (UBSan traps on)
./build.ps1 run          # build + run on http://127.0.0.1:8080, assets + module CSS live from disk
./build.ps1 test [flt]   # build + run every *_test.c (flt = substring of test/file name)
./build.ps1 release      # optimised Windows build
./build.ps1 pi | pi32    # static Linux binaries for the Raspberry Pi (aarch64 | armv6), ~2.2 MB
./build.ps1 linux        # static x86-64 Linux (pi/pi32 also run on any ARM Linux)
./build.ps1 mac          # macOS 11+: build/hemma-mac-arm64 + hemma-mac-x64 (cross-compiled, not yet run on a Mac)
```
- `$env:HEMMA_SKIP_MODULES="media,tv"` leaves module folders out (someone else's work in progress).
  media needs av and tv needs media: skipping one skips the ones that depend on it.
- `$env:HEMMA_BUILD_DIR="build-book"` builds into a private folder (parallel workers); SQLite objects
  are shared in `build/obj/<target>/` and compile once (~1 min; again when their flags change - a
  `.flags` stamp sits next to the object). Test scratch folders go to `<build dir>/test-tmp/` and are
  removed after a passing run (kept, and named, after a failing one).
- Size: release builds use -ffunction-sections/-fdata-sections + --gc-sections (macOS: -dead_strip) and no
  unwind tables; SQLite is built -Os without its JSON functions (FTS5 stays). 2.6 -> 2.2 MB incl. gz twins.
- Dev mode (`--web web`): editing web/ or the CSS of a compiled-in module reloads open browsers.
- PowerShell can swallow compiler errors: when a build fails without output, run it from Bash:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 build 2>&1 | tail -30`.
- Windows locks a running exe: stop your server before rebuilding into the same folder.
- On Linux/the Pi/macOS: `make`, `make test`, `make run`, `make SKIP="tv"`; `sudo make install` (systemd
  Linux only; a Mac uses deploy/local.hemma.plist - docs/macos.md).
- Platform code stays in core/plat.c + core/lan.c, each with Windows / Linux / macOS (`__APPLE__`) paths.
  After touching them, cross-compile every target (`./build.ps1 all`) - only Windows runs the suite here.
- Local dev sign-in: the gitignored `.env` sets `HEMMA_ADMIN_USER` / `HEMMA_ADMIN_PASSWORD` (dev only;
  production uses `/etc/hemma/hemma.env` from install-pi.sh). Copy `.env.example` to start.
- Pi-like limits on Windows: `zig cc -O2 tools/pisim.c -o build/pisim.exe`, then
  `build/pisim.exe --model 3 -- build/hemma.exe ...` (1 core capped to ~Pi 3 speed, 512 MB;
  models zero, zero2, 3, 4, 5). Prints peak memory and CPU time every 10 s.
- Real media for checks stays outside the repo (and off a nearly full boot drive); the maintainer's
  local notes say where it lives on their machine.
- Real-media checks (never part of the suite; the owner's media is read-only apart from `.web.mp4` copies):
  `HEMMA_TEST_SHOW=<show folder> ./build/test.exe media_real` lists how the library sees that folder;
  `HEMMA_TEST_MKV=<file.mkv>` / `HEMMA_TEST_AC3=<file>` run the av decoders over a real file.

## How the live UI works

Every open tab is a *client* with a URL. After any action (or presence change,
or once a minute) the core re-renders each client's page; if the HTML hash
changed it sends `["morph", html]` over the WebSocket and wire.js diffs it into
the DOM (attribute-level, so typing/focus/scroll survive). Navigation is
`["url", ...]` + morph. Modals and filters live in the URL. Actions are
`<form data-a="mod.verb">`; without the wire they POST to `/a/mod.verb`.
Sign-in/out are plain POSTs to `/login` `/logout` (cookies need real HTTP responses).

The event loop sleeps in poll() until there is work: a tick about once a second while screens
are open, every 5 s when none are (≈0.2 wake-ups/s idle, measured). A module that needs its tick
sooner asks from its tick with `app_wake_in(ms)` (net does while sweeping, uploads while sending).
`app_changed()` re-renders every screen; `app_changed_where("/media")` only screens on that path
(progress bars). Other threads end the poll wait with `server_wake()`; a worker that finished
something to show (an upload, a scan) calls `app_tick_soon()` so the modules' tick runs now
instead of at the next idle tick (a wake alone doesn't run the tick).
A `nav`/`hello` re-renders only that screen (plus `/` and `/people`, which show who is where;
the shell's "Here now" names people only); navs are rate-limited per screen. Every page carries
its hash (`<html data-h>`): the first `hello` after a load sends it, so an unchanged page isn't
sent twice. Static files are served gzipped (`Content-Encoding`, `Vary`) to browsers that accept
it: the build embeds a `.gz` twin of each text file and av.wasm (~120 KB -> ~30 KB first load).

## Accounts

- A `Person` is a household member; with `username` + `pass` they can sign in.
  Roles: `ROLE_MEMBER`, `ROLE_ADMIN` (manages people), `ROLE_DISPLAY` (read-only, e.g. a wall tablet).
- `person_count()/person_at()` = real people only (display accounts excluded) — use for pickers,
  "who's home", etc. `account_count()/account_at()` = every account (household admin).
- Sessions: random 128-bit cookie `hemma` (HttpOnly, SameSite=Lax); only its SHA-256 is stored.
  `session_create/session_end/sessions_drop`. Signing in again in another tab keeps that session.
- Secret salt (`HEMMA_AUTH_SALT`, the pepper in every password hash and recovery code): a new install makes
  it itself - `auth_ensure_secret()` in app_boot writes 256 random bits into the settings file (`--env`,
  default ./.env; created if missing, 0600 via `file_write_private`, an empty `HEMMA_AUTH_SALT=` line gets
  the value) - but NEVER once any account has a password (a new salt would lock everyone out: it warns).
  Never stored in the data folder or the database. The Pi installer still writes it into
  /etc/hemma/hemma.env (root:hemma 0640: Hemma can read it, not change it).
- First admin: the **welcome page** (core/auth.c `setup_page`, POST `/setup`) is shown to every signed-out
  visit while `auth_setup_open()`: salt set, setting `auth.bootstrapped` unset AND no admin who can sign in.
  It needs the one-time **setup code** first (`auth_setup_code()`: HEMMA_SETUP_CODE or setting auth.setup_code,
  logged at start, never shown on a page; wrong ones -> `/setup?e=setup`, throttled; cleared on success).
  The POST re-checks (single-threaded loop: racing submissions can't both win), same-origin, throttled per
  IP; success sets `auth.bootstrapped`, signs in, makes recovery codes, redirects to `/people/codes`. Once
  bootstrapped it never reopens (a deleted admin -> "Nobody can sign in" page pointing at the docs).
  Startup logs `No admin yet - open http://<addr>/ ...` while it's open. User docs: docs/accounts.md.
- Env bootstrap (`HEMMA_ADMIN_*`, headless alternative) unchanged: ONCE, only while no admin who can sign
  in exists; sets `auth.bootstrapped` (closes the welcome page). Deleting/renaming that admin is final.
  `HEMMA_ADMIN_RESET=1` + a new password still re-creates an admin (one-shot; drops its sessions).
- Recovery codes (table `recovery_code`, api_hidden): 8 per person, 12 symbols of
  `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (60 bits) shown `XXXX-XXXX-XXXX`; stored only as
  HMAC-SHA256(server salt, "recovery:<id>:<code>"); deleted when used. `auth_codes_new(person, sess)` keeps
  the plain codes in memory for that session (30 min or until `auth_codes_seen`); people module shows them
  at `/people/codes` (+ `/people/codes.txt`). Making new ones needs the password (`people.codes`). Display
  accounts and people without a sign-in have none (`auth_codes_drop` on removal / sign-in cleared).
- Forgot password: `/forgot` (username + code + new password twice) -> `auth_recover()` shares the sign-in
  throttle budget, answers alike for unknown users; success = `auth_set_password()` (ends EVERY session),
  fresh session, `/people?recovered=1` shows codes left.
- Server-side reset: `hemma --reset-password USER --env F --data D` (auth_reset_cli, own SQLite connection)
  writes a temporary password hash into raw table `auth_reset`; the running server notices via
  `PRAGMA data_version` in `auth_tick()` (or `auth_start()` at boot), applies it, drops sessions, sets pref
  `auth.temp` (sign-in then lands on /people with "choose your own"). On the Pi run it as `sudo -u hemma`.
- Sign-in throttle: per (IP, username) 5 tries, plus per account across all IPs, plus per IP across all
  usernames (10: each guess is a PBKDF2 hash on the event loop, ~1 s on a Pi 3); locks 1/2/4/8/15 min,
  forgiven after 30 quiet minutes - and never evicted earlier (full tables don't drop fresh failures; a
  new IP waits while the IP table is full). All checked BEFORE hashing. Recovery-code guesses and the
  current-password check (`auth_recheck(p, pw, c->ip, ...)`: people.password / people.codes) count too.
- Passwords: your own only via people.password (asks the current one, ends your other sessions);
  people.save refuses `password` for yourself. An admin setting someone else's ends their sessions.
- Signed-out requests never reach module pages, actions or `http` hooks (unless `MOD_PUBLIC`), and
  signed-out/revoked screens get no `app_notify` toasts. A signed-out WebSocket (hello/nav/act/close)
  only ever re-renders its own screen. `/api` is for members and admins (display accounts: 403).
- Log lines go through `log_clean()`: control characters become visible escapes (no forged lines).

## Writing a module (the contract — see core/app.h)

```c
#include "../../core/app.h"
#include "../../core/ui.h"
static void page(Ctx *c, Buf *b) { ui_head(b, "Things", NULL); ui_head_end(b); ... }
static void act_add(Ctx *c, Fields *f) { ...; tbl_put(&T_thing, &t); ctx_ok(c, "Added"); }
static const Action actions[] = {{"things.add", act_add}, {0}};
static const Module things = {.id = "things", .title = "Things", .icon = "list", .nav = 50,
                              .init = init, .page = page, .actions = actions, .widget = widget};
MODULE(things)   /* self-registers: no shared file to edit */
```
- **Pages**: `page()` renders `/<id>` and `/<id>/...`; read `c->path`, `ctx_q()`. Set `c->bare` for
  fullscreen pages. Modals = URL state (`?edit=3`) + `ui_modal(b, title, close_href)`.
- **Actions** mutate data, then `ctx_ok/ctx_fail/ctx_redirect`. They run inside one SQLite
  transaction; the core re-renders every screen afterwards. Never hand-craft DOM updates.
  `c->me` is always set in actions (signed in, not a display account); check `is_admin(c)` when needed.
  `app_notify()` toasts everyone else, `app_notify_person()` one person; `activity_add()` feeds home.
- **Live inputs**: `<input data-live="mod.preview" name="q">` sends a "live" action on typing;
  store per-screen state with `ctx_set_live()` and read it in `page()` with `ctx_live()`.
- **Data**: describe records with `FIELD(...)` (`FIELD_SECRET` = never in /api) + `TABLE(...)`,
  `store_register()` in `init`. Each becomes a SQLite table (new fields → new columns
  automatically). Reads are in-memory. Use `db.h` for big/queryable data. Keep ids, not row pointers.
- **Widgets**: `widget(c, b, WIDGET_HOME | WIDGET_WALL)`. Must cope with `c->me == NULL` and with a
  display account. **Agenda**: `agenda(c, from, to, out)` + `agenda_add()` puts timed things into
  shared calendars; read them with `agenda_collect(c, from, to, except_id, &ag)`.
- **Sidebar foot**: `side(c, b)` renders a small block above the profile in the sidebar (phones: under
  the sections on /more). Every page calls it - cached numbers only, no disk/network work (media's
  storage meter refreshes its cache from `tick` every 30 s).
- **Raw endpoints**: `http()` hook (files, streams, JSON) — only called for signed-in requests.
  Bodies over 1 MB are not buffered: the hook gets `req->streaming` (body NULL, `content_length`
  set) and must take the socket with `conn_detach(conn, req, &d)` — `d.body` holds the bytes that
  already arrived, `d.unsent` any queued output (send it first) — and read the rest on a thread
  (see media/upload.c). Otherwise the core answers 413 and closes. Same hand-off for streaming out
  (media/stream.c). Never block the loop: `conn_detach` doesn't.
- `c->me` and any row pointer can move when a table grows (`tbl_put` reallocates): keep ids, copy
  what you need into locals before a put. The core re-looks up `c->me` after every action.
- **CSS**: `modules/<id>/<id>.css`, appended to the core stylesheet. **Prefix every class and id
  with `<id>-`** (one shared bundle: generic names collide). Reuse the shared vocabulary in
  web/app.css unprefixed: .card .btn .chip .chips .chip-r .check .seg .tick .quick .preview .rows
  .row .badge .hued .field .field-row .stack .modal* .empty .toolbar .notice .muted .small .center.
- **Rules**: no `#ifdef _WIN32` (use plat.h/lan.h); don't block the event loop (threads for disk
  scans/streaming, results handed back on `tick`); check `app_testing()` before opening sockets or
  starting threads; don't edit core/ or web/ for one module's needs — ask for a core change instead.
- **Tests**: `modules/<id>/<id>_test.c` with tests/harness.h: `th_boot()`, `th_person()`,
  `th_act(person, "mod.verb", "a=1&b=2")`, `th_page(person, "/mod?x=1")`, `th_halt()`.
  Anonymous = person 0 → expect the sign-in page (`data-mod="auth"`). For sign-in tests call
  `auth_configure("salt", 1000)` (low iterations keep tests fast).

wire.js markup hooks: `form[data-a]` · `[data-live]` · `[data-auto]` (submit on change) ·
`[data-esc]` (Escape) · `[data-key="/"]` (shortcut) · `[data-static]` (never morphed) ·
`a[data-native]` (full load) · `form[data-keep]` (don't reset) · `form[data-confirm]` (asked before
any submit, socket or not) · `form[data-upload="/base"]` (files: POST base/plan, then one PUT per file
to base/<batch>/<i>?at=<offset>, resuming from GET base/<batch>/<i>; a todo entry `[i, at, end]` sends
only the bytes before `end`; `{i, f}` = convert file f here first (web/conv.js Workers, half the cores,
max 8, a few ahead of the upload), POST ?size= / ?original=1, then PUT the result as entry i; a `conv`
form field is dropped when the browser can't run WebAssembly; the server renders progress) ·
`video[data-resume|data-report|data-next]` · `[data-start="id"]` (open a horizontal scroller at that
child, once per URL — e.g. today's column on a phone).
Phones (≤600 px): the tab bar shows the first 4 sections + "More" (`/more`, core/ui.c `TAB_SLOTS`);
wide grids scroll sideways with a sticky first column (hours, channels). Check new pages at
390×844 (chrome-devtools `emulate` "390x844x3,mobile,touch"): no sideways page scroll.
Morphing keys siblings by `id`: give list items and cards that come and go an id, or render new
cards *after* existing ones (otherwise an unkeyed sibling gets rebuilt and loses typed input).

## Conventions & gotchas

- C11 (`-std=gnu11`), `-Wall -Wextra`, zero warnings. 4-space indent, `snake_case`.
- Fixed-size char arrays for record strings; copy with `str_copy` (UTF-8 safe).
- HTML only via `H()`/`html_esc` (escapes by default; `%r` = trusted raw HTML). H() knows only %s %r %u (URL-
  encoded) %d/%L (+ width, e.g. %02d) %X (a 64-bit id as 16 hex digits) %c %%: anything else (`%016llx`, `%lu`)
  is printed as-is WITHOUT taking its argument, so every later argument shifts - a crash. Use snprintf for those.
- Don't call a static helper the same as a core function (e.g. `file_read_at` clashed once).
- The debug test build traps on undefined behaviour (UBSan): never left-shift a negative value
  (multiply instead); it caught three such shifts in the H.264 encoder.
- `far` and `near` are macros on Windows (windef.h): don't use them as variable names.
- Older TV browsers (Samsung Tizen before ~2025 lacks `color-mix()`, before ~2023 `aspect-ratio`
  and `inset`): give every `color-mix()` a plain declaration first, write `inset` as
  top/right/bottom/left, and back `aspect-ratio` with `@supports not` sizes. Plain `http://` only:
  a browser that tries https gets hung up on (logged) - tell people to type `http://`.
- HTML pages are served with a CSP (`script-src 'self' 'wasm-unsafe-eval'` - the latter only lets
  pages compile WebAssembly, for the upload converter): never add inline `<script>` or `on*=`
  handlers; inline `style=` attributes are allowed. HEAD is handled by `conn_reply` (no body).
- Dev mode serves web/ from disk; built assets that aren't in web/ (av.wasm) come from the binary.
- The server faces every device on the LAN: Host must be an IP, a single-label name or a private
  suffix (.local .lan .home .home.arpa .internal …) — more via `HEMMA_HOSTS` (DNS-rebinding guard).
  Per-device connection cap (32; streams/uploads handed to threads don't count), a shared 32 MB budget
  for request bodies / WebSocket messages still arriving (past it: 503 + close; `server_set_buffer_budget`
  in tests), output backpressure, stall timeouts; keep new code bounded too.
- Bind 127.0.0.1 for dev servers (0.0.0.0 pops a Windows Firewall prompt). Never send Wake-on-LAN to
  a device without asking the owner.

## Media notes

- Library naming (names.c/library.c): the show folder names the series (walks up past season,
  disc and extras folders; the root's own name when the root is the show). MakeMKV disc rips
  (`season N/Disc - K/X1_tNN.mkv`) are numbered in disc → title order; per-disc "play all" titles
  and bonus clips become `role` extras (listed under Extras, never in episodes or TV channels).
  The maintainer's real test show (7 seasons, 178 episodes, DVD + HD rips) is the reference.
  Several episodes in one file (MediaName.episode_end): S01E01-E02, S01E01E02, S01E01-02, S01E01.E02,
  S01E01+E02, "S01E01 & E02" / "and" / "och", 1x01-1x02, 1x01-02, and "01-02 - Title" in a season
  folder. Across a joiner word the next number must follow directly ("S01E04 - E10 Title" stays a
  title). Rows show "1–2" (.media-ep-nn) and series totals count every episode. Names are parsed
  again from the path at every start (never cached), so parser changes reach existing libraries.
  Movies vs series: episode markers / season folders -> series; "Title (Year)" -> movie; folders named
  Series/Serier/TV Shows/Anime or Movies/Films/Filmer or Home videos/Family/DCIM lean the guess.
- Streaming is direct play (bytes as stored, Range requests, worker thread per stream, `sendfile`
  zero-copy on Linux). Browsers can't decode MPEG-2 (the owner's DVD seasons) and Chrome/Firefox
  not AC-3: that's what modules/av is for (owner wants it built in C, not ffmpeg).
  In the test show, seasons 1, 2, 3, 5 = hard-telecined film (IVTC -> 23.976p), season 6 = progressive
  29.97p (pass). The DVD rips: 720×480, 29.97 fps, interlaced (top field first), AC-3 2.0 192 kbit/s 48 kHz
  (Dolby Surround flagged; some episodes carry a 2nd commentary track) in laced 32 ms frames;
  video tracks not flagged default. All 183 tracks decode exactly (ac3: ~1,400× real time on a
  PC core, ~96× under pisim model 3). Target for "convert once": MP4 with H.264 + AAC-LC (the
  one audio codec every browser, iPhone, TV and Chromecast plays). MPEG-2 decode: ~1,100 fps on one PC core, ~59 fps
  under pisim model 3 (2× real time, 3 MB) — live decode fits a Pi 3, live H.264 *encode* doesn't.
- Uploads (admins, /media/upload): written as `<name>.part`, renamed when complete, fsynced;
  resumable; refused up front when the disk lacks room (+1 GB reserve). On the Pi the service
  needs write access to the media folders (install-pi.sh takes them as arguments).

## Status (2026-10-11)

Done: core (+ login with a first-run welcome page, recovery codes and a server-side reset, SQLite store,
agenda, JSON API), people/household, home, book, plan, board, wall, net, media (library, player, Range
streaming, progress, uploads that convert on the sending device, disc rips, pictures), tv.
398 tests green (1.6M checks), incl. end-to-end socket tests (core/app_http_test.c, core/auth_setup_test.c;
media/upload_test.c with a dropped-and-resumed 3 MB upload).
Platforms: the Windows build and the Pi aarch64 build run in daily use (a Pi 3B+ since 2026-10-10). Linux
x86-64, Pi armv6 and macOS (arm64/x64) builds compile without warnings but have never run, and the Makefile
has never been run (no Linux/macOS machine here) - CI on GitHub would close that gap.
Review passes: core protocol, auth/app/wire, feature modules — all findings fixed with regression tests.
Pre-publication sweep (2026-10-11, five reviewers: hygiene/docs, security, upload+converter, performance,
quality/portability): no critical/high security issue; fixed with tests - a per-address sign-in budget
checked before hashing, throttle tables that can't be flushed, a 32 MB budget for buffered bodies,
nav/hello re-rendering only their screen, log lines can't be forged, /api closed to display accounts,
admins change their own password only under Your account; uploads: stale .part files, late progress
reports, failed files, room for the original, per-upload Stop, one file in one upload, copies folded
into a Blob in the browser; converter: sound-track choice, AC-3 at 48 kHz only, no silent copies,
index room cut from ~7 MB to ~1.6-3.8 MB per copy, ~4x faster comb detection; gzip assets, smaller
binaries, faster morphs on old engines (gaps()), versioned pictures cached for good.
Measured under pisim model 3 (≈Pi 3 CPU, 512 MB): idle 0.2 wake-ups/s; 8 viewers × 8 Mbit/s
streams ≈10% of the core; ceiling ≈1.5 Gbit/s before CPU runs out — the network is the limit
(Pi 3B 100 Mbit, 3B+ ~300 Mbit over USB 2 shared with the disk; Pi 4/5 gigabit for 4K remuxes);
peak memory 8.6 MB. Real upload of 3.6 GB at disk speed (~112 MiB/s) in 9 MB RAM.
Measured on a real Pi 3B+ over Wi-Fi: 8 viewers × 8 Mbit/s with 0 errors, 91 Mbit/s single stream,
2.4 MB RAM; a 46 GB library of converted copies serves fine from its SD card.
Converter: a 22-minute DVD episode takes ~282 s on one PC core natively, ~374 s as WebAssembly (Node and
Chrome); copies are 190-480 MB per episode vs ~1 GB originals; video bit-identical native vs wasm, AAC
differs only by float rounding. A whole 7-season show (171 files) converted with 0 failures, and its
disc-rip numbering checked season by season against the originals (play-all headers keep featurettes
out of the episodes when only copies are uploaded).
Admin-only: people management, home name, media library folders + uploads, network discovery.
Owner decisions (2026-10-11): the Network page's changes (device edits, owners, Wake-on-LAN, the
presence window) are admin-only - members only look (done, net.c admin_only); the sign-in page keeps its
"Welcome back" guess. Plain http on the LAN stays (HTTPS weighed - own domain + Let's Encrypt, a home CA,
or a proxy, with mbedTLS for TLS - and declined for now; old TV browsers need http anyway).
Done 2026-10-11: one-time setup code for the welcome page (owner asked; README explains it in detail):
auth_setup_code() - 8 symbols of the recovery-code alphabet ("7KQ4-M2XP", 40 bits), HEMMA_SETUP_CODE from the
settings file (install-pi.sh writes one via `hemma --gen-setup-code` and prints it) or Hemma's own in setting
auth.setup_code, logged at start next to "No admin yet", never on a page; setup_post checks it first (ct_equal,
throttled), success clears it.
Done 2026-10-11: storage meter (owner asked: "used space - space left" above the profile) - media's `side`
hook, admins only, links to /media/settings: free space + bar + "used of total" per disk holding a library
folder (path_disk_space; folders on the same disk shown once), cached and refreshed every 30 s from tick
(and right after the folders change); bar turns warn at 85 % used, danger at 95 % or under 2 GB free.
Done 2026-10-11 (afternoon, owner asked): uploads skip episodes/films the library already has under another
name; HEVC MKVs get a Convert button and become MP4 copies Safari plays (checked on a real 1080p x265 10-bit film:
3.6 GB in 55 s, plays and seeks in Chrome); admins remove films from the watch page (media/remove.c); README
has a "Why C?" section. Later: "keep only the most compatible version" of an original + its copy (button +
optional automatic after converting; 53 of 53 real pairs matched), subtitles attach to copies-only videos (they
didn't before), the copy's first read shows on the progress bar.
Then: an x on "Continue watching" cards (/media shelf + the home card; media.c going_card, action media.forget)
drops that person's progress in the video - others' stays; display accounts get no x.
Open for the owner: license, donation links, project URL (README/docs placeholders).
Next ideas: builder-style UI helpers / less raw HTML (owner asked), MKV→MP4 remux on the fly
for Safari/iPhone, "Open in VLC" links, router integration, DLNA, recurring calendar events,
API tokens for scripts, "tune in" on the TV's browser via QR code, HEVC/DTS in the converter.
