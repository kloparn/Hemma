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
- **Every PR raises the version** (`HEMMA_VERSION` in core/app.h) and gets its own CHANGELOG.md section
  (owner, 2026-10-12): **patch** = fixes only, **minor** = something new or changed, **major** = a big
  breaking change in how things work. Bump it in the PR itself, not after the merge.

## Layout

```
core/            framework with a stable API for modules
  base.*         buffers (always NUL-terminated), strings, UTF-8, time/calendar, hashing, logging
                 (log_set_sink: called on ANY thread after log_clean - the sink locks and never logs;
                 g_log_stderr = false keeps tests quiet)
  plat.*         the ONLY place with #ifdef _WIN32 besides lan.c and base.c's two clock calls (sockets,
                 files, dirs, threads, random, env)
  lan.*          LAN facts: interface/gateway, ARP neighbours, probes, Wake-on-LAN, reverse-DNS packets
                 (Windows: iphlpapi; Linux: /proc + /sys; macOS: routing-table sysctl, parsed by the
                 portable lan_parse_rt_addrs so it's unit-tested on every platform)
  codec.*        SHA-1, SHA-256, HMAC, PBKDF2, Base64, hex, URL encoding
  json.* http.* ws.* server.*   protocol stack + event loop
  html.*         H() — HTML formatter that escapes by default (%s escaped, %r raw, %02d padding ok)
  store.*, db.h  typed tables kept in RAM, persisted in SQLite; db.h = raw SQL (+FTS5)
  when.*         natural-language dates/times (English + Swedish) for quick-add bars. A day that can't be
                 ("31 sep", "29/2/2027") or a day range ending before it starts ("12/10-10/10", "until 5/10")
                 is refused as a whole: when_parse() returns false, gives the words back and says why in
                 When.refused (book/plan show it). "14-12/10" (the month before) only when it lasts <= 14
                 days. An end date without a year takes the start's year, or the next one past New Year
                 ("28/12-2/1"). A time after the last day of a written-out range ends it ("fri till sun 17").
                 Evening days ("tonight", "ikväll", "inatt", "imorgon kväll", "fri night", "this evening")
                 read 5-11 as pm, 12 as midnight, 1-4 as after midnight; a time belongs to the day word right
                 before it; "tonight" typed between 00:00 and 05:00 means the night we're in.
                 when_resolve() settles clock changes itself (wall_time): a time in the skipped spring hour
                 moves forward keeping its length, the repeated autumn hour means its first pass, end > start
  env.*          .env loading (a non-empty real environment wins; in the file the first line WITH a value
                 counts - an empty "KEY=" never hides a later one; a repeated key is logged; a bad line is
                 skipped and logged, the rest still loads; "KEY= # note" is empty). env_line_key() = the key
                 a line sets, read the same way (auth.c uses it to find the HEMMA_AUTH_SALT= line, BOM too)
  auth.*         passwords (PBKDF2 + server salt), throttling, admin bootstrap, welcome/sign-in/forgot pages,
                 /login /logout /setup /forgot, recovery codes, --reset-password (see "Accounts")
  app.*          the live core: modules, routing, sessions, render→diff→morph, actions, presence,
                 settings, per-person prefs, activity feed, shared agenda, JSON API (/api), the health
                 API (health_add/health_step/health_collect, for Module.health), app_config(),
                 app_addresses() (LAN IP + <hostname>.local), public `GET /healthz` -> "200 ok" (after
                 the Host guard, nothing else in it), the startup banner (where to open it, setup code
                 hint, where the log is). HEMMA_VERSION lives in app.h; HEMMA_VERSION_FULL adds the
                 commit (build.ps1/Makefile pass -DHEMMA_COMMIT=<sha>[-dirty]) - raise the version per release
  ui.*           components: shell, icons, avatars, cards, forms, modals, time formatting
  main.c         command line (--gen-salt, --env, --web ...)
modules/<id>/    one folder per feature; folder name == module id (home book plan media tv board net people
                 screens system)
  screens/       the TV view and walls: screens an admin builds from the Blocks modules offer.
                 Tables `screen` + `screen_row` (position, block id, choice arg, custom heading); seeded once
                 each: a "TV" (setting screens.seeded; TV_ROWS: media.continue, tv.onnow, media.recent,
                 media.series, media.movies) and a "Wall" (screens.wall_seeded - households from before walls
                 get one at their next start; WALL_CARDS: plan.today, book.now, plan.days, plan.shop,
                 board.notes, tv.onnow, media.recent, home.latest). /screens = list (everyone opens, admins
                 edit/new - TV or Wall), /screens/<id> = full-screen view, /screens/<id>/edit = the builder
                 (Up/Down/Remove, heading, choice, "Add a row"/"Add a card", "Shows on" = display accounts'
                 PREF_START, one screen per account). GET /wall (the old wall module's address; the home and
                 Household buttons link it with data-native) = 302 to the first wall (screens_first), else
                 /screens - the http hook, so only full loads. Rows/cards are left out when their block is
                 unknown (builder: "missing"), not for this kind, an admin module's (for members) or renders
                 nothing. A TV: bare, always dark, 5% overscan, [data-arrows]. A WALL (owner, 2026-10-12: "make
                 it plain better"): bare, always dark, ~16px base (module card markup is px-sized); the top
                 band = clock + date, now/next (agenda_collect now..end of tomorrow, 3 at most: running ones
                 "Now ... until", then times, "Tomorrow" ones), who's home (home/away only when
                 presence_known, online = Hemma open); cards (<section class=screens-card><h2>) in CSS columns
                 (column-width 300px, inline column-count = cards, max 4: content-height masonry); no cards
                 with anything: "All quiet". Look only - no forms on a wall. Edit/Exit faint, bottom right.
                 WHO'S WATCHING (owner,
                 2026-10-12): a display account with ctx_viewer() == 0 gets the avatar picker (screens.viewer,
                 .display; who=0 = Switch); the pick lapses after SCREENS_IDLE_S (4 h) without use (the TV
                 session's `seen`), checked when rendering and on tick; people on their own devices are always
                 the viewer. Walls are nobody's: no picker.
                 Wall blocks (SCREEN_WALL): plan.today (events + chores due, plan-wall look), plan.days (the
                 next 7 days, 8 rows; an event once, on its first day, "until Sun"), plan.shop, book.now
                 (each thing by name: Free / "Adam · until 17:00", then what's coming), board.notes,
                 home.latest (activity by people only: who != 0), plus media.recent and tv.onnow.
                 Tile vocabulary (web/app.css, shared): .tiles (sideways row; margin 0 -14px + padding 14px is
                 room for the focus ring), a.tile / button.tile (2:3 poster; .wide = 16:9), .tile-art (an <img>
                 covers it, else text on a --h gradient), .tile-t, .tile-s, .tile-bar > i[style=width:N%],
                 .tile:focus (ring + scale). TV sizes live in screens.css under .screens-tv.
  system/        admin-only (MOD_ADMIN) System page: checks (core + every Module.health hook), a getting-
                 started list ticking itself from data (also an admin home card with the next step), the live
                 log (log.c: ring of 2000 lines fed by the core log sink - installed by a constructor, so the
                 first lines count; <data>/hemma.log written from tick at most every 2 s, no fsync, rotated
                 at ~1 MB to hemma.log.1; the previous tail read back behind a "Hemma restarted" divider;
                 start/stop marks in the file reveal a crash), help for how THIS instance runs (systemd,
                 launchd, Windows, a terminal), /system/log.txt download, a home card while a check is at
                 needs-a-look or worse. app_main halts the modules when it can't listen (the reason reaches
                 the file). media/health.c and net.c report their checks through the hook.
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
                 Writes x.web.mp4.conv.part (convert_part_path - never an upload's x.web.mp4.part) and never
                 renames over an existing copy. enqueue skips a copy already on disk, one an upload claims
                 (upload_claims), or a second original making the same copy. A copy being converted is the
                 server's (convert_making); one only queued leaves the queue when an upload brings it
                 (convert_give_way, after the plan is accepted). ROOM (media/room.c): a worker (one at a time,
                 g_pick) claims the first waiting file whose original's size fits, then re-checks; the rest
                 wait while others run (and, with auto-delete on, for finished ones whose originals are about
                 to go), and with none running they're set aside as CQ_NOROOM (one line on the card: "Make
                 room, then convert again"); the action sets aside at once what can't fit even alone. Scans
                 remove orphaned *.conv.part. Lock order: g_pick, then upload.c's g_lock, then g_cl. Stop flag
                 + progress are atomics; change signatures are uint64_t (UBSan). Tests queue without threads
                 (convert_test_start/finish).
  media/room.c   disk room shared by uploads and conversions: MEDIA_RESERVE (1 GB) always stays free (the
                 database often shares the disk), and running jobs hold what they may still write (an upload
                 its unsent bytes, a conversion its original's size minus what it wrote), per disk (same
                 total size, free within 64 MB). media_room(), media_room_text(), media_test_free_space().
                 Uploads never take a file the server's converter is making (convert_making): skipped at the
                 plan; a quiet upload whose copy the converter took meanwhile gets PUT ok without writing
                 (UpFile.on_server; GET says at=size; its .part deleted) and ?size= -> {"skip":true}. Every PUT
                 checks the rest of its file fits (else 507 + stop). upload_claims = files being written +
                 queued files of a live upload (running, or planned in the last 30 s). A x.web.mp4.part older
                 than the server's start is never resumed (an older build's converter wrote that name).
  net/           the device list is capped (NET_MAX_DEVICES 1024): when full, the longest-unseen devices the
                 household didn't set up (name/owner/kind/note/WoL) make room, those someone opens Hemma from
                 last; never the router (only the ONE device at the router's address now: find_router),
                 devices online now or listed this round; else newcomers are left out (logged once). start()
                 trims; admins have "Forget unknown devices not seen for a week" (net.tidy). New devices: one
                 activity line an hour (later rounds add to its count), a toast at most once an hour, not for
                 MACs evicted lately. /net shows 100 rows per step (?n=). Name questions carry fresh random ids
                 (DNS and mDNS separate); the first response from a sender who may answer closes the question -
                 a wrong id or "no such name" too (net_dns_packet). Presence from devices is labelled a guess
                 (ARP can't be authenticated: any device can borrow a phone's MAC).
  media/ + tv/   ONE Watch section: tabs Home | Movies | Series | Live TV | Folders (media_lib_head() in
                 media/media.h; tv's pages draw it too; Live TV only while tv is built). tv = .nav 0,
                 .parent "media". Recently added groups a series' episodes that arrived within a week of
                 each other (media_recent). Blocks (media/tiles.c): media.continue (TV), media.recent (TV +
                 wall), media.series, media.movies, media.series_one (choices = every series by hex id;
                 next-up first), tv.onnow (TV + wall). Drawn posters: media_tile_art / media_art_style
                 (--h/--h2/--h3 from a title hash, comma hsl() and no calc() for old TVs). TV MODE
                 (?tv=1&back=<path> on /media/watch/<id>, /media/series/<id>, /tv/<n>): bare, always dark,
                 Back first (data-esc), back checked by media_safe_back (else "/"); the player autoplays +
                 autofocuses, resumes for the viewer, data-next keeps tv/back; a channel: ArrowUp/Down
                 change channel, 1-9 pick one. PLAYERS (owner, 2026-10-12: full screen must survive the
                 next episode): our own controls (media_controls -> ui_player, subtitles from media_subs;
                 tv: a live UiPlayer), the box (#media-box / #tv-screen) goes full screen, the <video>
                 keeps one id with data-static="<file>" (tv: "<file>-<airing start>"; the overlay has the
                 same key) so the next one morphs into it; an
                 episode with a next one gets media_upnext() (MEDIA_UPNEXT_S = 30: "Next episode in N s",
                 Play now, Hide; tv=true: data-arrows + autofocus). On a screen (kind != 0) tiles link to TV mode with back =
                 c->path. Everything personal is ctx_viewer(c); media.progress (live), media.watched and
                 media.forget are .display and do nothing without a viewer; display accounts get real
                 posters and streams (/media/* only asks for a session).
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
                 conv.c bounds samples per track (2M; with a stated length 300 pictures / 120 sound blocks
                 per second; AvConvJob.max_samples lowers it) and refuses MPEG-2 whose picture size changes
                 part-way - both `unsupported` (the browser then sends the original). Timestamps beyond ~13
                 days count as missing (MAX_TS). DeintIn carries width/height; deint_push refuses any other.
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
tools/convfuzz.c libFuzzer target over av_convert (MKV in memory -> MP4 in memory); -DCONVFUZZ_MAIN = a
                 replay/mutate driver that builds with zig on Windows (build lines at the top). Seeds:
                 `HEMMA_FUZZ_SEEDS=<dir> ./build/test conv_fuzz_seeds` (generated, never committed)
tests/           test.h (framework), runner.c, harness.* (boot the core without a server)
deploy/          hemma.service (hardened systemd unit; waits for time-sync.target), install-pi.sh (any systemd
                 Linux, despite the name: enables systemd-time-wait-sync with a 90 s limit, starts the service,
                 waits for /healthz, prints addresses + setup code + next steps, or the journal tail and exit 1),
                 local.hemma.plist (macOS launchd agent). The Windows guide runs Hemma as Local Service (S-1-5-19,
                 icacls on C:\Hemma and the media folders), never SYSTEM.
docs/            user guides: raspberry-pi linux windows macos home-network ssh accounts troubleshooting development
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
- CI (.github/workflows/build.yml, every push): `make` + `make test` on Ubuntu and macOS; the suite again
  under ASan + UBSan (clang, leak checks off); a 90 s libFuzzer run over the converter (tools/convfuzz.c).
  None of it can run on this Windows PC (no ASan/libFuzzer in zig here): push and read the CI result.
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
`{"t":"log","url":...,"m":...}` from a signed-in screen = the browser's own trouble, logged as a warning
("browser of <username> (<ip>) on <url>: ..."), 20 per connection (Client.logs): wire.js report() sends
window errors, unhandled rejections, a video's `error`, and a video meant to play (autoplay, live, or play
pressed) that hasn't fired `playing` 25 s later (watch(): readyState, networkState, paused, buffered, the
user agent) - a TV's browser has no readable console; read it on the System page or the journal.
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
  A session ends (`session_over`, checked at every lookup, rows pruned hourly) a week unused, a year
  unused with "keep me signed in", and - for admins - `ADMIN_SESSION_DAYS` (30) after signing in however
  much it's used (owner, 2026-10-12: plain http, a captured admin cookie must die; the cookie's Max-Age
  says 30 days for admins too). The owner wants more conditions on top later - ask before adding any.
- Sign in with the username OR the name (`person_for_sign_in`: the username first, else the one account
  that can sign in and goes by that name, folded - "Åsa"/"åsa"; two such accounts = name doesn't work).
  Throttling keys on the resolved username, so name and username share one budget. Same on /forgot.
- "Welcome back, X" (the address's last person) has "Not X? Sign in as someone else" (`/login?other=1`).
- Secret fingerprint: setting `auth.salt_check` = 8 bytes of HMAC(salt, constant), refreshed whenever a
  password is made or checks out; `auth_salt_mismatch()` -> a warning at start + a hint on failed sign-ins.
- Keep an admin: people.save/people.delete refuse any change after which no admin can sign in, counted
  AFTER the change (people.c admins_after/keeps_an_admin). Pickers' ids go through `person_real(id)`
  (display accounts can't be booked for / assigned).
- Secret salt (`HEMMA_AUTH_SALT`, the pepper in every password hash and recovery code): a new install makes
  it itself - `auth_ensure_secret()` in app_boot writes 256 random bits into the settings file (`--env`,
  default ./.env; created if missing, 0600 via `file_write_private` - which writes through a symlinked
  settings file to its target - an empty `HEMMA_AUTH_SALT=` line gets the value, BOM or not) - but NEVER
  once any account has a password (a new salt would lock everyone out: it warns).
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
  Hashing budget: checks for (address, user) pairs that never signed in share a quarter of the loop's time
  (HASH_SHARE, bursts of 4 s); past it auth_login says LOGIN_BUSY ("Lots of sign-in tries right now. Wait
  N seconds", /login?...&busy=1), which counts against the address like a wrong try (a flood locks its own
  addresses); known pairs (in memory: since the last start) always go through. Re-entering the password
  in a signed-in browser restarts the session's `created` (an admin's 30 days). The
  account-wide lock is logged like the address lock. The
  setup code's throttle waits (FULL_WAIT_S) when the pair table is full instead of failing open.
- Passwords: your own only via people.password (asks the current one, ends your other sessions);
  people.save refuses `password` for yourself. An admin setting someone else's ends their sessions.
- Signed-out requests never reach module pages, actions or `http` hooks (unless `MOD_PUBLIC`), and
  signed-out/revoked screens get no `app_notify` toasts. One exception, for `http` hooks only: an
  ADDRESS KEY (`ctx_url_key(c, path, out)`, app.c url_key/session_by_url_key): `<path>?k=<session id>-<128
  bits of HMAC(server salt, session token hash + path)>`. A GET/HEAD without a live session cookie whose
  key fits its exact path runs the module endpoints as that session (live, not session_over); an
  address no endpoint takes goes on as signed out - a key never opens a page, an action, /api or /ws.
  Why (2026-10-13): a Samsung TV's browser (an update that week) lets its video player fetch the
  <video>'s file WITHOUT the page's cookie (the navigation had it, the player's `Range: bytes=0-` didn't),
  so Hemma answered with the sign-in page and the TV said "decode error" - every video, Live TV, every
  Hemma version. media_file_src/media_sub_src put keys on video and subtitle addresses (media, tiles,
  tv); pictures need none (the browser fetches those itself). No stored session (the harness's
  app_ctx) = no key = the plain address. A signed-out WebSocket (hello/nav/act/close)
  only ever re-renders its own screen. `/api` is for members and admins (display accounts: 403); tables
  with `api_owner` (pref, media_watch) show a member only their own rows, admins all (owner, 2026-10-12).
  Signed-out WebSockets: 48 in all, 4 per address (`stranger_room`), past that the upgrade gets 503.
  `MOD_ADMIN` modules exist only for admins: nav, /more, pages ("not found" for others) and http hooks.
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
  store per-screen state with `ctx_set_live()` and read it in `page()` with `ctx_live()`. The action
  must be flagged in its table, `{"mod.preview", act_preview, true}` (`Action.live`): a "live" message
  naming anything else runs nothing. Live actions get no ack and don't re-render other screens (call
  `app_changed()` yourself if one changed shared data, as media.progress does when a video ends).
- **Data**: describe records with `FIELD(...)` (`FIELD_SECRET` = never in /api; `T.api_owner = "person"`
  in init = /api shows members only their own rows) + `TABLE(...)`,
  `store_register()` in `init`. Each becomes a SQLite table (new fields → new columns
  automatically). Reads are in-memory. Use `db.h` for big/queryable data. Keep ids, not row pointers.
- **Widgets**: `widget(c, b, WIDGET_HOME)` - a card for the home dashboard (walls are screens of blocks,
  not widgets, since 0.6.0). Must cope with `c->me == NULL` and with a display account. **Agenda**:
  `agenda(c, from, to, out)` + `agenda_add()` puts timed things into shared calendars (book: bookings,
  plan: calendar events); read them with `agenda_collect(c, from, to, except_id, &ag)` - a module that
  shows its own items itself reads with except = its id (plan's calendar does).
- **Sidebar foot**: `side(c, b)` renders a small block above the profile in the sidebar (phones: under
  the sections on /more). Every page calls it - cached numbers only, no disk/network work (media's
  storage meter refreshes its cache from `tick` every 30 s).
- **Health**: `health(c, h)` reports checks for the admin System page with `health_add(h, HEALTH_OK|
  INFO|WARN|BAD, title, detail, todo, link)` and getting-started steps with `health_step(h, done, title,
  detail, link)`. Called for admins only (also on the home page): cached facts only. `system` never links
  other modules - only this hook.
- **Admin-only module**: `.flags = MOD_ADMIN` hides it from non-admins everywhere (nav, /more, pages,
  http hooks, side(), widgets, agenda items, the /api action list); its actions still check `is_admin`.
- **Blocks for screens** (the TV view, wall tablets - modules/screens arranges them): `.blocks` =
  `{id "<mod>.<name>", title, kinds SCREEN_TV|SCREEN_WALL, render, choices}`, terminated by {0}.
  `bool render(c, b, kind, arg)` draws one row's content WITHOUT its heading (false = nothing to show:
  the row is left out); `choices()` (optional) lists what the block can show (e.g. one series) for the
  builder's pick, passed back as `arg`. Look one up with `block_find(id, &owner)` - also how one module
  shows another's row without linking it (Watch's home renders "tv.onnow" with kind 0 = an ordinary
  page: plain links instead of TV-mode ones).
- **Section parent**: a module with `.nav = 0` and `.parent = "media"` lights up that entry (tv's
  channel pages live under Watch).
- **The viewer**: personal media state (progress, continue watching, watched ticks) belongs to
  `ctx_viewer(c)` - the signed-in person, or on a display account (the living-room TV) the person picked
  under "Who's watching?" (`ctx_set_viewer`, stored in the session; 0 = nobody yet). Actions with
  `.display = true` (`{"media.progress", act_progress, true, true}`) may run from a display account -
  watching only; everything else stays "view-only" for them. A display account's "/" shows the page
  in its pref `PREF_START` (e.g. "/screens/2"; the query goes along) - use absolute links on such pages.
- **Raw endpoints**: `http()` hook (files, streams, JSON) — only called for signed-in requests
  (or ones with an address key: an address a media player fetches goes out with `ctx_url_key` - see
  Accounts). Bodies over 1 MB are not buffered: the hook gets `req->streaming` (body NULL, `content_length`
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

wire.js markup hooks: `[data-arrows]` (remote control: arrow keys move focus to the nearest focusable in
that direction - Left/Right stay on the row, Up/Down go to the nearest row; after a load or morph focus
returns near where it was, else [autofocus] / the first focusable; inputs keep their arrows - don't put it
around a <video>) · `video[data-onair]` (a live broadcast - owner, 2026-10-12: no controls; plays at
data-resume + the time since the page came, a pause resumes, winding/speed snap back, a browser blocking
sound starts it muted; the same element stays from programme to programme, and so does its sound) ·
`[data-player="<video id>"]` (ui_player(): the player's OWN controls - owner, 2026-10-12: one
set of buttons in every browser and for the TV's remote. The overlay sits in the video's box with the same
data-static key as the video; hidden + the video's `controls` until wire.js wires it (no-JS fallback). State
on the overlay: data-on (bar shown: a move, a key, paused; 3 still seconds of playing hide it - not while
paused, a menu is open, the mouse is on the bar, or a live channel is muted), data-paused, data-wait,
data-muted. Parts [data-p]: play back fwd seek at dur mute vol subs; [data-sub="<srclang>"] menu items
(the pick carries into the next episode, v._sub). Keys (registered BEFORE [data-arrows]): Space/K, Left/
Right ±10 s, M, F; on a computer they stay the video's even on a focused bar button; UiPlayer.tv puts
data-arrows on the bar (a remote moves along it; Down/Up on the picture go to the bar, Up on the seek bar
back). Keys come by e.key, else by keyCode (KEY_CODES: a TV's browser may say "Unidentified"), the
remote's MediaPlay/Pause/PlayPause/Rewind/FastForward included; Enter with nothing focused plays (live: the
sound, or starts a channel that never did). The spinner (data-wait) ends on playing/pause/seeked/canplay/
loadeddata or moving time - else a TV that blocks autoplay could keep the big play button hidden; paused
shows .pl-big, live channels too. A video `error`: .pl-err (UiPlayer.trouble - media: the HEVC sentence for
HEVC copies; tv: "The next one starts at HH:MM") instead of spinner/play - but first TWO PLAYERS (owner,
2026-10-13: "default to our own, if that does not work ... the original video player"): any `error` in
Hemma's player -> nativePlayer(): a NEW <video> with every attribute + <track>s copied, data-native,
`controls` (not for live), autoplay, data-resume = where it was (live: where the channel is now), the old
element's src removed + load() (a TV has one decoder; a TV refused the SAME element after its decode error
- error 4), replaceWith; media() wires it (progress, next episode, live rules). plWire puts data-native on the
overlay: bar/big/spinner gone, only .pl-natfs (a second [data-fullscreen] button, top right) - the video has
controlslist=nofullscreen, so full screen stays the BOX's: the "Next episode" card shows in it and the next
episode (or another switch: a new element inside the box) keeps it. Our clicks/keys leave a data-native
player alone (its own controls). Only when that fails too: .pl-err. Logged both ways ("plays in the browser's
own player" / "in the browser's own player too"). The next video (renewal) tries Hemma's player again.
(Built while chasing the owner's Samsung TV failing every video with PIPELINE_ERROR_DECODE; the real cause
was its player fetching without cookies - see "address key" under Accounts. The fallback stays, owner's
choice.) A mouse click on the picture plays/pauses, a tap shows/hides the bar. Live (.pl-live): Live badge,
sound ("Sound off" in words and the bar stays while muted), full screen) · `[data-fullscreen="id"]` (the
player's full-screen button: #id - the box, so the bar and the card show too - full screen and back;
double-click on the video too; arrives hidden, shown where the browser can - an iPhone can't put a page
element full screen: its video's webkitEnterFullscreen (Apple's player) instead; while full screen
`<html data-idle>` hides the pointer after 3 still seconds; Escape then only leaves full screen, never
[data-esc]. Chrome driven by DevTools (the chrome-devtools MCP) keeps ANY full screen inside its window:
check real full screen in a normal browser) ·
`[data-upnext="S"]` (the page's "next episode" card for its video[data-next]: unhidden in the last S
seconds, `[data-count]` = seconds left, `[data-hide]` puts it away for that video, a link in it reports
the video watched BEFORE the nav goes; an [autofocus] in it takes the focus from the video) ·
`form[data-a]` · `[data-live]` · `[data-auto]` (submit on change) ·
`[data-esc]` (Escape) · `[data-key="/"]` (shortcut) · `[data-static]` (never morphed; `data-static="v"`:
skipped while v stays, a new v morphs the SAME element in place and settles it again - how a player
keeps full screen into the next episode/programme/channel: stable ids media-video / tv-video) ·
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
- Morphing keys elements by their id ATTRIBUTE (`getAttribute('id')` in wire.js): `form.id` returns a
  form's `<input name="id">` - it once made every form with an id field rebuild on each morph (typed text lost).
- A live/act message's `url` becomes the screen's url on the server (app.c on_ws_message): one sent
  after a nav but carrying the old page's url moves the screen back (it undid "Play now" once). Send
  such reports before the nav. A focused element a morph hides stays document.activeElement until the
  next frame: wire.js `unfocused()` counts it as nothing focused.
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
  Per-device connection cap (32; streams/uploads handed to threads don't count); at the total cap (400) a
  newcomer takes the place of a slot holder - an idle keep-alive connection, else a half-sent request -
  from the address holding the most (server.c slot_holder; screens/WebSockets never), a shared 32 MB budget
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
  resumable; refused up front when the disk lacks room (+1 GB reserve, media/room.c - conversions keep
  it too). On the Pi the service needs write access to the media folders (install-pi.sh takes them as
  arguments).
- Rescan: admins, plus members an admin ticks on Library settings (setting `media.rescanners` = member ids;
  only the admin-only action `media.rescanners` writes it; display accounts, admins and removed people
  never count). Owner's rule (2026-10-12).

## Status (2026-10-12)

Done 2026-10-12: GitHub issues #3-#15 (an outside review's findings; issue #3 the owner's own) with tests,
then a second review of those fixes (also fixed): sign-in by name + "Not X?", admin sessions 30 days,
hashing budget, slot eviction, signed-out WebSocket cap, live-action gating, /api owner rows, rescan
permission, settings-file parsing, last-admin guard, converter size-change OOB read + sample bounds,
CI sanitizers + converter fuzzing, conversions keep room + never share a .part with uploads, net list cap
+ router/DNS hardening, quick-add date refusals/evening words/DST, Windows as Local Service, Pi clock sync,
version 0.3.0 + commit stamp + CHANGELOG.md. New (owner asked): the System page (checks, getting started,
live log, help), clearer installer and startup output, /healthz. 456 tests green (1.64M checks).
0.3.0 merged to main (PR #16), not tagged yet (the owner runs `git tag v0.3.0` + a GitHub release from
CHANGELOG.md). 0.4.0 (PR #17, the GUI redesign round 1) = Watch absorbs TV, Screens (the TV view with
"Who's watching?"), grouped "Recently added", per-series poster colours, live TV can't be paused.
0.5.0 (PR #18 + the version bump after it) = full screen survives the next episode, a "Next episode in N s"
card, full screen for Live TV (survives programme and channel changes), Hemma's own player controls
(ui_player + wire.js [data-player]) instead of each browser's, and the fix for live TV playing on, unseen,
after leaving it. 0.6.0 (branch feat/update-wall) = the wall as a Screen kind (top band, card columns, wall
blocks from plan/book/board/home, seeded once, /wall forwards), the old wall module and WIDGET_WALL gone,
plan's events in the shared agenda. Next ideas: calmer home cards (hide empty ones), touch on the wall
(ticking the shopping list - needs .display actions), more wall blocks (weather is out: LAN only).

Done: core (+ login with a first-run welcome page, recovery codes and a server-side reset, SQLite store,
agenda, JSON API), people/household, home, book, plan, board, screens (TV + wall), net, media (library, player, Range
streaming, progress, uploads that convert on the sending device, disc rips, pictures), tv.
(2026-10-11: 398 tests green (1.6M checks)), incl. end-to-end socket tests (core/app_http_test.c, core/auth_setup_test.c;
media/upload_test.c with a dropped-and-resumed 3 MB upload).
Platforms: the Windows build and the Pi aarch64 build run in daily use (a Pi 3B+ since 2026-10-10).
GitHub CI (.github/workflows/build.yml) runs `make` + `make test` on ubuntu-latest and macos-latest on
every push (green). Pi armv6 builds compile without warnings but have never run.
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
