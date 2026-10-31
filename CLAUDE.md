# Hemma — household LAN hub in pure C

Hemma ("at home") is a self-hosted page for our home network: book shared things
(car, laundry, guest room…), plan (chores, shopping list, calendar), a notice board,
network/presence view, a media portal (movies/series from a local drive) and
"90's TV channels" that run continuously on a shared clock. README.md is the
front page for people choosing Hemma - SHORT AND PLAIN (owner, 2026-10-27: "the wall of text to explain
something? Way to much for a simple user ... why you should select us. I love the images"): what it is,
the pictures with a few sentences each, Why Hemma, Is it for you, Try it, Get started, Need help, a few
lines for developers. No feature lists, settings, tables of setup details or technical talk there: those
go into docs/ (docs/features.md = everything it does, section by section; the guides for the rest), and
the README links them. This file is the working contract (for agents: every rule, in detail). For
people: ARCHITECTURE.md (how it fits together), CONTRIBUTING.md (how to send a change),
docs/decisions/ (why - one record per big choice).

Runs on a Raspberry Pi in production; developed on Windows 11 with `zig cc`.

## Owner preferences (keep honouring these)

- **Everything in C** — backend *and* frontend. No JS frameworks, no npm, no CDNs.
  The browser runs `web/wire.js`: the transport (WebSocket, DOM morphing, form/link
  interception) plus the two things only a browser can do - the video PLAYER (its own
  controls, resume, progress reports, the next episode, Live TV's "now") and the UPLOAD
  CLIENT (plan, PUTs, resuming, converting in Workers). Issue #49 (2026-10-21): that's ~1,700
  lines, so it's linted (eslint) and tested in a real browser (tests/browser); the
  household's rules live in C wherever they can - a page must work without wire.js (the
  server puts a video's start in its address: `#t=`) - and the few that only the browser can
  keep are listed under "the player" below and each has a browser test. No other app logic
  goes in JS. Uploads run Hemma's C converter compiled to WebAssembly (`av.wasm`, fed by
  the tiny Worker `web/conv.js`) - still C, just running on the sending device.
  THE ONE EXCEPTION: `apps/android/` (2.0; owner, 2026-10-28: "a web-app pretty much, only allow local
  connection ... it tries to find the local-network of the hemma application"; docs/decisions/0011) - plain
  Java, a thin window (WebView) onto Hemma's own pages that adds only what a WebView can't do. No household
  logic there either.
- **Server-driven UI**: C renders HTML; changes are pushed as DOM morphs over
  WebSocket to every open screen. Pages also work without JS (links + POST forms).
- **Fast and light**: single-threaded event loop, small static binary for the Pi.
  The only third-party code is SQLite (vendored, public domain) - and, as data, FFmpeg's two DTS vector
  tables in third_party/ffmpeg/dts_vq.h (LGPL 2.1+, its README; owner, 2026-10-24: "FFmpeg's, own file" -
  the DTS specification leaves them out of every edition).
- **The internet only when an admin ticks it** (1.4; owner, 2026-10-27: "if internet is available ... if the
  user has checked the box", "It can be grayed if the server does not have online activity available to it"):
  Hemma itself fetches nothing from outside the home but media/lookup.c's genres (TVmaze, OMDb with the admin's
  key) and - 2.5, a box of its own - pictures for titles without one (TVmaze, Wikipedia), off by default, over plain
  http (core/fetch.c - no TLS library; never anything secret that way) or, where a service answers only https, through
  the computer's own curl (fetch_https; owner, 2026-10-30: "I do not want it to be setup with another key", "Use curl
  for https"; docs/decisions/0012), only titles and years sent. Updates still go through the root helper (hemma-upgrade,
  curl). Anything new that goes online gets its own box, off by default, greyed while unreachable.
- **Modular**: features are self-contained modules so several workers can build
  in parallel without touching each other's files.
- **Tests**: co-located `*_test.c`, auto-registered. Cover parsers, protocol code,
  module logic and rendered pages (via the harness). Keep the suite green.
- **Local database**: SQLite (`data/hemma.db`), chosen over DuckDB (C++, huge,
  analytics-oriented). People/accounts, preferences and module data live there.
- **Login in front of everything**: only the sign-in (and first-run welcome) page is public. The
  first admin is created on the welcome page (or, headless, from the settings file); the admin
  creates everyone else.
- Household audience: friendly, phone-first UI in English, Swedish and Spanish (issue #46; the browser's
  language, or each person's own choice), quick-add input understood in all three.
- **This file is shareable** (it goes in the repo): nothing machine-specific here - no paths on a
  dev PC, addresses, MACs, hostnames of anyone's machines, credentials or deployment diaries. The
  maintainer keeps those in private local notes (outside the repo).
- Keep this file updated as things change (owner's request).
- **A written review for `core/`** (issue #48, owner 2026-10-21; docs/decisions/0007): a PR changing core/
  gets a review written on the PR before merging - an AI review counts when its findings are posted there
  and answered. PRs aim under ~1,500 changed lines (translations, test data aside), features and refactors
  apart; .github/pull_request_template.md holds the checklist. The branch's Pi build runs on the real Pi
  before the merge (staged as usual), since merging releases it.
- **Video formats grow with their fuzzing** (issue #50, owner 2026-10-21: "keep supporting more ... when
  we need it, minor bumps"; docs/decisions/0006): a format the converter takes is listed in
  modules/av/conv.h AV_CONV_CODECS (av_conv_codec() - the converter's checks consult it), and
  conv_fuzz_seeds fails until a seed carries it; a new decoder gets an avfuzz target too. And its row in
  docs/video-formats.md (owner, 2026-10-27: "a supported video list ... formats we help the client to
  convert to"): what plays as it is, what's converted into what, the file-name words, "Not yet" - keep it
  true to convert.c convert_need, probe.c judge_in and AV_CONV_CODECS.
- **Every PR raises the version** (the VERSION file: MAJOR.MINOR.PATCH, digits only - semver.org; owner,
  2026-10-19: "easy to find") and gets its own CHANGELOG.md section
  (owner, 2026-10-12): **patch** = fixes only, **minor** = something new or changed, **major** = a big
  breaking change in how things work - or a milestone the owner names (1.0.0; 2.0.0 = the Android app, owner
  2026-10-28: "a nice new soft-launch where an app is available"). Bump it in the PR itself, not after the merge. The section is headed
  "## X.Y.Z (<day>)" (the day the PR is made). MERGING RELEASES IT (issues #19, #52; owner, 2026-10-19: "an
  action that reacts to the version ups"; a separate .version file was weighed and left out - HEMMA_VERSION
  stays the one place - then the owner asked for it in a file of its own, following a standard: VERSION): .github/workflows/release.yml on a push to main touching VERSION or CHANGELOG.md
  -> job check (VERSION; a release or tag vX.Y.Z already there = skip) -> ./build.ps1 test, pi,
  pi32, linux, mac on windows-latest (setup-zig 0.17.0; av.wasm included) -> tools/ci/bundle.sh
  (downloads + SHA256SUMS, each SIGNED - SHA256SUMS.sig by `hemma --sign` with the GitHub secret
  HEMMA_SIGNING_KEY (issue #73; no secret = a publish fails, a dry run warns) - + notes: that version's CHANGELOG section, REQUIRED, + a downloads table + the
  commits since the newest v* tag before HEAD (git log --no-merges; HEMMA_PREV overrides; publish checks out
  fetch-depth 0) + a compare link) ->
  `gh release create vX.Y.Z --target <sha>` (the workflow makes the tag; concurrency group "release" queues
  a second merge). "Run workflow" by hand = a dry run (the downloads kept as a run artifact). Downloads:
  .tar.gz each (keeps the executable bit). NO WINDOWS DOWNLOAD (owner, 2026-10-19): Defender's machine
  learning quarantines hemma.exe as Trojan:Win64/CobaltStrike.K!MTB (a false positive; debug builds and
  tiny test tools were hit too) - it comes back once signed or cleared with Microsoft; docs/windows.md
  builds from source and says how to allow a self-built one. Guides link releases/latest/download/<file>.

## Layout

```
core/            framework with a stable API for modules
  base.*         buffers (always NUL-terminated), strings, UTF-8, time/calendar, hashing, logging
                 (log_set_sink: called on ANY thread after log_clean - the sink locks and never logs;
                 g_log_stderr = false keeps tests quiet)
                 free_on_loop(p)/buf_free_on_loop(&b) (1.5.1, docs/decisions/0010): a worker thread hands back what
                 the EVENT LOOP allocated (its job, conn_detach's bytes) instead of free()ing it; server_run frees them
                 each turn (free_on_loop_now(), also app_halt). musl (the Pi's static builds) never reused a block
                 another thread freed while the loop kept allocating: 2 KB upload jobs held 100 MB on the Pi after an
                 afternoon. A worker never grows (realloc) a loop buffer either - it copies it into its own.
                 thread_spawn's start record goes back the same way. glibc/Windows never showed it: check memory
                 on a musl build (x86_64-linux-musl under WSL, or the Pi)
                 lang_now()/lang_use(): the THREAD's language (issue #46) - weekday_name()/month_name()
                 follow it (Swedish and Spanish in lower case; ui_day capitalises a line start)
  i18n.*         LANGUAGES (issue #46; owner, 2026-10-20: English, Swedish, Spanish - "a helper that
                 selects from the browser's language, but you can select the language you want"). The
                 English is the key: T("...") = it in the thread's language (English when there's no
                 translation), TN(n, one, other) (1 = one, the rest other, in all three), N_("...") marks
                 text translated LATER where it's shown. T/TN carry format_arg, so printf checking still
                 sees the literal. Tables: `<name>_lang.c` beside the code - `static const Tr TR[] =
                 {{en, sv, es}, ...}; TRANSLATIONS(name, TR)` (a constructor registers it; the index is built
                 at app_boot, first registered wins). i18n_pack/i18n_vpack keep an English format + its
                 values for later ("fmt \x1e val \x1f val"), i18n_pack_add appends a sentence (\x1d between),
                 i18n_unpack shows it in the reader's language (a plain old text as it is); a "%'lld" value
                 is grouped by the language when shown (i18n_group: 2,944 · 2 944 · 2.944). i18n_accept =
                 the best of ours in an Accept-Language (q-values), i18n_parse "sv-SE" -> LANG_SV,
                 i18n_name = each language named in itself. TC("time", "until %s") / NC(...) = one English text
                 translated by context ("hasta las 17:00" / "hasta el dom"): the key is "time\x04until %s", English
                 shows what follows \x04 (table entry {"time\x04" "until %s", ...}). core/i18n_test.c: every table whole (no empty
                 translation, the English's format specifiers in the same order, the same inline tags, one
                 English never translated two ways) and EVERY T("/TN(/N_( literal in core/ and modules/
                 (not av/, tests, _lang.c) has both translations - a C-literal scanner; wire.js's T('...')
                 must be in core/ui.c WIRE_TEXTS. HEMMA_I18N_AUDIT=1 ./build/test.exe i18n_audit lists what
                 reads the same in every language on the main pages.
  plat.*         the ONLY place with #ifdef _WIN32 besides lan.c and base.c's two clock calls (sockets,
                 files, dirs, threads, random, env); plat_run(argv, timeout) (2.5) runs a program by its full path and
                 waits (posix_spawn with /dev/null as its stdio and SIGPIPE back to default / CreateProcessW with no
                 window; past the time it's killed: -1) - threads only; plat_quote_args = Windows' command-line quoting
  lan.*          LAN facts: interface/gateway (LanIface.link: Wi-Fi or a cable - Windows the adapter's IfType, Linux
                 /sys/class/net/<dev>/wireless or a real device of type 1, macOS SIOCGIFFUNCTIONALTYPE, not yet seen on a
                 real Mac; unknown -> the page names the interface), ARP neighbours, probes, Wake-on-LAN, reverse-DNS packets
                 (Windows: iphlpapi; Linux: /proc + /sys; macOS: routing-table sysctl, parsed by the
                 portable lan_parse_rt_addrs so it's unit-tested on every platform)
  codec.*        SHA-1, SHA-256, SHA-512, HMAC, PBKDF2, Base64 (encode + base64_decode), hex, URL encoding
  ed25519.*      Ed25519 (RFC 8032) for signed releases: ours, after TweetNaCl (public domain); its field
                 constants worked out at the first use (d, sqrt(-1), the base point from y = 4/5) instead of
                 typed in; carries multiply (no left shift of negatives: UBSan); verify refuses S >= L
  release.*      signed releases (issue #73): RELEASE_KEYS = the public keys (hex) releases may be signed with
                 (the maintainer's since 0.21.1; empty = nothing verifies and --sign refuses);
                 release_verify_dir(dir, partial) = SHA256SUMS.sig is a known key's signature over SHA256SUMS,
                 its lines "<64 hex>  <plain name>" only, every listed file there (partial: at least one) with
                 that SHA-256, nothing unlisted, no links/folders; release_key_new (never over a file),
                 release_sign (refuses a key not built in). CLI: hemma --gen-signing-key FILE | --sign FILE
                 ($HEMMA_SIGNING_KEY) | --verify-release DIR [--partial] (main.c release_cli, before the env)
  json.* http.* ws.* server.*   protocol stack + event loop
  html.*         H() — HTML formatter that escapes by default (%s escaped, %r raw, %02d padding ok)
  store.*, db.h  typed tables kept in RAM, persisted in SQLite; db.h = raw SQL (+FTS5). store_open checks the
                 file first (PRAGMA quick_check) and reads every table to its end (load() checks the last step):
                 a DAMAGED file (issue #37; owner, 2026-10-14: "rescue and carry on") is copied aside untouched
                 (hemma.db.damaged-<day>-<hhmm>[-k], its -wal too; a copy that fails half-way is deleted; the
                 newest DAMAGED_KEEP = 3 stay) and SQLite's recovery extension (sqlite3_recover, third_party/
                 sqlite/recover.c: page by page, like the shell's .recover - a broken header, a short file, a
                 damaged interior page; issues #87 #88) writes every row it can read into hemma.db.rescue.
                 Orphans (rows whose tree page above was the broken one) land in lost_and_found(rootpgno, pgno,
                 nfield, id, c0...): place_orphans() puts each PAGE back into the one table its rows fit (c0
                 NULL = the id alias, text/integer per column, ids not taken) when that's one of ours - the
                 file's other integer-id tables count as places it may fit too (a cache's page never lands in
                 the household's data); the rest stay in lost_and_found. The report counts only rows on pages
                 that fit one of ours AND something else; a page fitting none of ours (media_file: a text id,
                 WITHOUT ROWID) is only logged ("a cache's, most likely").
                 Ids: keep_sequences (the old sqlite_sequence, or +1000 past the top when rows were lost or the
                 count is unknown). THE CENSUS: <data>/hemma.db.counts ("table rows" lines) written at every
                 healthy open, store_maintain and a clean store_close - what a rescue compares with. REFUSED
                 (issue #87: a rescue of nothing started Hemma as a new household, the welcome page over the
                 data): no rows, person rows 0 while the census had people (or no census), or a census >= 20
                 and under a tenth kept -> the file is left as it was (the copy and .rescue removed),
                 store_refused() + store_refusal() (a sentence for people). Else hemma.db.rescue replaces it and
                 store_rescue_report() says it in the household's words ("2,944 of 3,000 rows were kept - notes
                 on the board: 2,944 of 3,000", Table.what labels; "N more ... in the table lost_and_found")
                 -> app_boot keeps it as setting SETTING_RESCUED ("store.rescued") -> System "Database rescued"
                 check + an admin notice on every page until "Got it" (system.rescued_seen); while it's set the
                 welcome page stays closed, and with no admin who can sign in auth shows damaged_page to
                 signed-out visits (members: a Sign in link) and members get a notice on every page. A
                 busy/unreadable file is no damage: it fails to open. store_damaged() = a write failed with
                 SQLITE_CORRUPT (System says restart, not "disk full?"). A refused start (here or the format
                 guard) -> app_main appends the refusal to <data>/hemma.log between "- Hemma X started" and
                 "- Hemma stopped" (the System module isn't running yet) and returns HEMMA_EXIT_DATA (78;
                 hemma.service RestartPreventExitStatus=78: no restart loop). Tests: store_test.c's filled()/
                 page_of() damage real files; core/app_data_test.c.
                 BACKUPS (issue #35): store_backup(to) = VACUUM INTO over its OWN read-only connection (any
                 thread; WAL lets the household carry on), fsynced (VACUUM INTO doesn't), checked, to.part ->
                 to. store_check_copy() = quick_check + a person table (Hemma's tables = those with an INTEGER
                 PRIMARY KEY id, so no registration needed) + min_reader <= store_format(). PUTTING ONE BACK:
                 store_stage_restore(data, file, label, ...) checks a .db or a download's .tar (core/tar: hemma.db
                 + hemma-secret.txt, whose HEMMA_AUTH_SALT comes back for the CLI) and leaves it as
                 <data>/hemma.db.restore (+ .restore.from = the label); store_open's restore_staged() puts it in
                 place first thing (checked again, and writable - else hemma.db.restore-refused), keeping the
                 old data as hemma.db.before-restore-<day>-<hhmm> (RESTORE_KEEP 3, prune_copies) ->
                 store_restore_report() -> app_boot's SETTING_RESTORED -> System "Backup put back" until Got it.
                 hemma --restore FILE (main.c restore_cli) stages it the same way; run it as Hemma's user.
                 DATA FORMAT (issue #38): PRAGMA user_version = the newest StoreMigration in store.c's MIGRATIONS
                 (format 1 = the baseline); a start runs those past the file's format (after ensure_schema, in the
                 open transaction), keeping a VACUUM INTO copy (hemma.db.before-format-<n>) first when one has a
                 run() or breaks_older; store_meta.min_reader = the newest breaks_older format applied: an older
                 program refuses to start on such data, but reads files that only gained tables/fields. The
                 refusal names versions (issue #92): store_meta "program" = the newest Hemma that opened the data
                 (store_set_program(HEMMA_VERSION) in app_boot; stored as a BLOB - v is an INTEGER column, "1.2"
                 would turn into a number): "This data was last used by Hemma 1.2 (data format 3), and this Hemma
                 1.0 reads only up to format 1 ... Install Hemma 1.2 or newer again, or put back the copy made
                 before the change: hemma.db.before-format-3". FIELD_WAS(T, member, type, "old", format) renames a
                 column with its data in that format; store_renames_unsafe() names one without a breaks_older
                 migration at or after it (app_data_test fails on it). Tests: store_test_migrations(list, n).
  when.*         natural-language dates/times (English, Swedish and - issue #46 - Spanish: hoy, mañana, pasado
                 mañana, "el lunes", "el próximo lunes", "5 de noviembre", "a las 14", "de 14 a 16", "esta noche",
                 "media hora", "en 30 minutos", "todos los martes", "cada dos semanas"; accents optional). Words
                 Spanish shares with Swedish or English ("el 12", "dom", "vie", "en 30 min", "ya", "14 a 16") read
                 Spanish only for a person using Hemma in Spanish (i18n_lang()) or inside an unmistakably Spanish
                 phrase; "el próximo viernes" = the next Friday to come (English "next fri" keeps next week's); "mar"
                 stays March. Refusals and when_describe's words are T()'d (a line start capitalised in sv/es).
                 For quick-add bars. A day that can't be
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
                 app_addresses() (LAN IP + <hostname>.local only when lan_mdns_announced(); the listen setting read
                 by plat.c listen_addr() like the listener: ""/"*"/0.0.0.0 = all, "localhost" = 127.0.0.1 - issue #99),
                 app_local_only() (127.x/localhost: System, Screens and the banner ask it), public `GET /healthz` -> "200 ok" (after
                 the Host guard, nothing else in it), public `GET /hemma.json` (2.0, after the Host guard; what the Android
                 app looks for) -> {"hemma": version, "id": app_instance() = setting core.instance, 16 hex made once at
                 app_boot, "name": the household's (the sign-in page shows it anyway)}; app_static_size(path) = a
                 built-in /static file's size, -1 = none (app_test_static in tests: the people card asks it for
                 /hemma.apk); session_in_app(s) = signed in from the app (its UA says "HemmaApp/": Session.agent
                 "The Hemma app on Android"), the startup banner (where to open it, setup code
                 hint, where the log is). HEMMA_VERSION = hemma_version() and HEMMA_VERSION_FULL =
                 hemma_version_full() (+ the commit) in core/version.c: build.ps1/Makefile read the VERSION file
                 (refused unless MAJOR.MINOR.PATCH) and pass -DHEMMA_VER=<it> + -DHEMMA_COMMIT=<sha>[-dirty]
                 (the Makefile to version.c alone: a new version or commit recompiles one file; a bare token,
                 stringified - digits only, so no macro can expand in it). Not a string literal any more:
                 no "Hemma " HEMMA_VERSION concatenation. core/version_test.c: VERSION == hemma_version()
                 and CHANGELOG.md's FIRST section is "## <version> (" (tests run from the repo top)
  ui.*           components: shell (first a "Skip to content" link to main#main[tabindex=-1] - issue #68), icons,
                 avatars (ui_avatar = role=img + the name, for one ALONE; ui_avatar_deco = aria-hidden, for one beside
                 the name - issue #69), cards, forms, modals (role=dialog aria-labelledby=modal-t; the backdrop link
                 tabindex=-1 aria-hidden), time formatting; ui_admins() = who a member
                 asks ("Anna (admin)", "Anna or Jonas (admins)", "an admin" past three - issue #58: no "Set up" links
                 to pages members can't change)
  qr.*           QR codes (byte mode, versions 1-10, levels L-H, best of 8 masks) -> qr_svg(); ours, not a
                 library (after ISO 18004 / Nayuki's reference). qr_test.c holds fingerprints of codes the
                 independent reader jsQR decoded back (137 cases, every version/level/mask, checked in Node
                 from a wasm32-wasi build - Windows Defender quarantines tiny unsigned test exes here)
  tar.*          the little of tar (ustar) the backup download and hemma --restore need: tar_add/tar_end,
                 tar_looks, tar_find (in memory; checksums checked; GNU tar reads ours)
  fetch.*        Hemma's one way out (1.4): fetch_get(host, port, path, max, timeout) = a plain-HTTP/1.1 GET, BLOCKING
                 (plat_resolve = getaddrinfo, tcp_connect_nb + poll, reads to the close) - threads only; Fetched =
                 status (0 = no answer, err says why), the body de-chunked, a redirect's location (not followed:
                 maybe https). fetch_parse alone is what fetch_test.c checks. No TLS: only services answering http://.
                 HTTPS (2.5): fetch_https(url, max, timeout) runs the computer's OWN curl (fetch_curl(): System32's
                 curl.exe by %SystemRoot%, else /usr/bin /bin /usr/local/bin /opt/homebrew/bin; NULL = none) with -sS -L
                 --max-redirs 3 --proto =https --proto-redir =https --max-time --max-filesize, -D/-o/--stderr into temp
                 files (TMPDIR|TEMP|TMP|/tmp, removed after) - the certificate checked by curl. Fetched.exit = curl's
                 exit code (6/7/28 = no network); fetch_curl_status reads the last answer's status from -D. Tests:
                 fetch_test_curl("" | path | NULL).
  main.c         command line (--gen-salt, --env, --web, --reset-password, --restore, --demo ...)
  zone.*         the time zone (issue #39; core/zone.h; series too: their start/end/until move, and a row a
                 series made at the start after a change counts as typed when its series was): zone_sig() = the offsets in January/July
                 ("+01:00/+02:00"; zone_test_sig in tests), kept as setting system.time_zone (the names
                 the System page used first). zone_boot() (app_boot, after the store, BEFORE the modules
                 start): a new signature -> every module's Module.rezone with z->days: the days they keep as
                 local midnights move to the new zone's midnight of the same day (rezone_day: a value that is
                 an old-zone midnight by one of its two offsets and not a midnight here; idempotent) - done
                 by itself, a day is a day - and the change is recorded in system.tz_changed
                 "<from>|<to>|<day>|<unix noticed>|<days kept>|<retimed: -1 = not yet>" (0.14's 3-field
                 records still read). TYPED times (timed bookings and events made before "noticed") move
                 back to the clock reading they were typed at only on an admin's choice: zone_retime(false)
                 = count + the first REZONE_NOTES previews ("Dentist · Tue 13 Oct 16:30 → 14:30", the System
                 page's offer), zone_retime(true) = once (system.tz_retime). rezone_time: the old zone's
                 summer told by this zone's, else by the EU's dates (exact without summer time, like a
                 Pi's UTC). rezone_span(z, days, made, &start, &end) = the usual hook body. Hooks: plan
                 (Task.due, all-day Event days, timed Event times), book (whole-day things' bookings as
                 days, others typed), board (a Note's "until" midnight). A NEW all-day field anywhere
                 needs its line in its module's rezone hook. Tests: plat_set_tz("UTC0"/"XXX-2"),
                 th_halt + th_boot_in(the same dir) = "Hemma restarted".
  feed.c         a person's calendar on their phone (issue #41): GET /cal/<id>-<32 hex>.ics (on_http,
                 before the session - no cookie; the Host guard first) = iCalendar (RFC 5545: CRLF, folded
                 at 75 octets never inside a UTF-8 letter, TEXT escaped, UIDs = hash64(module|link), all-day
                 as VALUE=DATE, timed in UTC, to-dos TRANSPARENT) of agenda_collect with Agenda.todos (plan
                 adds open to-dos with a day; an overdue one on today "(due Mon 5 Oct)"): what's for them or
                 for everyone (PREF_CAL_ALL "1": every booking too), a month back to a year ahead. Key =
                 128 bits of HMAC(server secret, "cal:<id>:<n>") - nothing stored; pref PREF_CAL "cal.feed"
                 = n, off = -n (a link turned off never works again), a new link n+1. Only people who can
                 sign in (person_real + pass). The UI is people.c's "Your calendar on your phone" card
                 (people.cal_make/_off/_all): webcal:// + http:// on Ctx.host (the Host the visitor used;
                 on 127.x a note + no QR code), a QR code of the webcal link.
  demo.c         hemma --demo (issue #31): main.c makes <TMPDIR|TEMP|TMP|/tmp>/hemma-demo-<random>/ with demo.env
                 (HEMMA_ADMIN_* = anna + a random demo-<hex> password, so the env bootstrap makes her; refuses
                 --data/--env) and data/; app_boot -> app_demo_seed(pw) once (setting demo.seeded) fills the
                 made-up household "Björkbacken" (Jonas, Maja, Leo, starters, bookings, chores, shopping,
                 events, notes) through the modules' OWN actions by name (app_action as anna/jonas; a step
                 that fails is logged "demo: ... didn't go in", never fatal) - so a module's change of action
                 or form shows up in core/demo_test.c, not as a silently empty demo. app_demo() -> a band on
                 every page (ui_shell .demo-band), the login on the sign-in page and in the startup banner.
                 Tests: app_test_demo(pw).
modules/<id>/    one folder per feature; folder name == module id (home book plan media tv board net people
                 screens system)
  book/          things to book (Resource) and bookings. Quick booking (book_quick_parse) matches a thing by its
                 emoji, name, its own "Other names" (Resource.aliases, comma-separated) and - while it's still named
                 like a starter - the starter's Swedish words (PRESETS .aliases: bastu, tvätt ... - issue #55); a
                 word that names nothing gets "I don't know 'x'. You can book: ...". Someone else cancelling or
                 moving a booking (issue #43) tells the person it's for and whoever made it: a toast plus a
                 `book_news` row (30 days, 20 each) on their Bookings page and home card until "Got it"
                 (book.news_seen). Resource.own_only (an admin sets it): only those two or an admin may cancel or
                 move (book_may_change; the booking dialog says who may). Resource.kids / .guests (issue #44, an
                 admin's "Children may book it" / "Guests may book it"; starters' PRESETS .kids/.guests; things
                 from before start closed): book_open_to(c, r), book_res_list(c) - a child or a guest sees and
                 books only those, for themselves, and changes only their own bookings. A thing open until
                 midnight shows "00:00" in its dialog, read back as 1440.
                 REPEATING (issue #47): BookSeries (table booking_series: the first start/end, repeat REPEAT_*,
                 until = last start day, made = occurrences 0..made-1 done, stopped) makes REAL bookings
                 (Booking.series) SERIES_AHEAD_DAYS (56) ahead - book_series_fill() at start, right after
                 save_booking makes one (quick "every tuesday": Quick.repeat; the form's rep/rep_until) and hourly
                 from tick; occurrence k = book_series_at() from the FIRST (add_days k*step; monthly month_shift
                 clamped: the 31st -> 30 Nov -> 31 Dec). One that book_check refuses (taken, hours) is left out +
                 a book_news line to whoever it's for. Cancel: one, or later=1 = this and every later one + the
                 series stopped (ui_form_confirm: no Undo). A series stops by itself when its thing/person is gone.
  board/         notes on the fridge door. PHOTOS (issue #47; owner, 2026-10-19: "shrink in the browser"): the post
                 box and the dialog have input[name=photo][data-shrink=1600] (shown only under html[data-ws=on]);
                 read_photo() takes "photo" = data:image/jpeg;base64,... (PHOTO_MAX 700 KB decoded, jpeg_size()
                 from the SOFn header, <= 4096 px) -> <data>/board/<rand_hex 8>.jpg (Note.photo, photo_w/h: pages
                 give width/height so a wall's fit doesn't jump); "photo_off" takes it away; a photo alone is a
                 note. GET /board/photo/<16 hex>.jpg (http hook, signed-in, immutable). photo_sweep(): files no
                 note has - all at start, hourly once an hour old (UNDO_S keeps a deleted note's for its Undo).
  plan/          to-dos (tasks.c), shopping (shop.c), the calendar (cal.c). TAKING TURNS (issue #47): a repeating
                 chore with Task.turns = person ids in order ("3,5,9"), .turn = whose place this time is, .swap = who
                 gave this time away (and so takes the next). Quick add "@anna,@erik" / "@anna @erik" (claim_people:
                 every @, in order) + a repeat; the dialog's "Taking turns" field = names (read_turns, errors at
                 "turns"). Ticking off spawns the next with plan_next_turn() (the swap first, else the next place
                 with a real person - someone removed is skipped) + a toast to them; plan.task_swap swaps with the
                 next in line or back. Rows say "next: Erik". REPEATING EVENTS: EventSeries (plan_event_series) makes
                 real Event rows (Event.series) a year ahead (a daily one 60 days) - plan_series_fill at start, on
                 creating and hourly from tick; one event changes/goes on its own, "Delete this and later ones"
                 (later=1, confirm) stops it. Agenda.todos (the calendar feed only) adds open to-dos with a day.
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
                 with anything: "All quiet". Look only - no forms on a wall, and no links either (issue #80:
                 blocks render kind == SCREEN_WALL as <div class="tile">, book's upcoming(..., links=false)
                 rows; plan's to-dos a .plan-dot, not a tick circle). Edit/Exit at .65, bottom right; admins also
                 get #screens-fitsay naming the cards that didn't fit (issue #97: not on the hall tablet); Exit
                 is left out when "/" would bring this display account straight back (display_home_is: its
                 PREF_START, else SETTING_DISPLAY_START - as core display_start()). Board notes are <li> rows so
                 a full Notes card shortens instead of going whole. FITTING (issue #77): the cards box is
                 [data-fit="screens-fitsay"] (wire.js) and, wider than 600 px, flex-bounded to the screen
                 (overflow hidden; a phone just scrolls): rows give way ("+N more"), then whole cards from
                 the end, then rows come back where they fit - checked at 1024x768, 1280x800, 800x1280,
                 1920x1080. Lists mark day headings data-fit-head and their own "+N more" data-fit-more="N"
                 data-fit-keep (book, plan). A display account without PREF_START opens the first wall
                 (setting SETTING_DISPLAY_START, kept by display_default() at start/create/delete; issue
                 #53) - none: the home page. /screens' "On the TV" steps (admins) show the address to type
                 (issue #61): screens_addresses() = app_addresses() asked from tick once a minute (never on
                 render; screens_test_addresses() in tests), the LAN IP big with http://, the .local name
                 for phones ("many TVs can't"), and a QR code of the IP (core/qr) for tablets/phones;
                 listening on 127.x says a TV can't reach it.
                 WHO'S WATCHING (owner,
                 2026-10-12): a display account with ctx_viewer() == 0 gets the avatar picker (screens.viewer,
                 .display; who=0 = Switch); the pick lapses after SCREENS_IDLE_S (4 h) without use (the TV
                 session's `seen`), checked when rendering and on tick; people on their own devices are always
                 the viewer. Walls are nobody's: no picker. Only a display account that DRIVES A TV picks
                 (screens_drives_tv: its PREF_START is a SCREEN_TV screen; issue #33 - the hall tablet could
                 pick anyone and read/rewrite/clear their Continue watching): others get no picker or Switch
                 and watch as nobody; forget_viewers() clears the pick from the sessions of display accounts
                 that stop driving a TV (Shows on, a deleted screen, and at start). THE GUARD (issue #90;
                 owner, 2026-10-17): screens_on_picker() = core privacy_on_tv (issue #42): PREF_ON_TV
                 ("screens.on_tv" "1"/"0", the /screens "You on the TV" card or Household -> Privacy; unset:
                 members and children on, admins and guests off); act_viewer refuses
                 anyone else, screens_viewer() drops a pick that's off the list since (an admin who unticked).
                 An optional PIN (PREF_PIN = hex of HMAC(salt, "pin:<id>:<4 digits>") via auth_mac, 128 bits
                 shown; set/cleared by screens.pin): the picker's entry is a link to ?pin=<id>, a numeric
                 password field posting who + pin; 5 wrong guesses lock that PERSON (every TV) for 1, 2, 4 ...
                 15 min (g_pin, forgiven after an hour), screens_pin_wait(). Setting a PIN or unticking
                 forgets the picks of you (forget_viewer_of). Display sign-ins end DISPLAY_SESSION_DAYS (90)
                 after signing in. Per-device pairing (a second device on the TV's login can pick too) is not
                 done: the PIN covers it for people who set one.
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
                 the file). media/health.c and net.c report their checks through the hook. TIME ZONE (issue
                 #39, part 1): start() keeps the zone as its UTC offsets in January/July (setting
                 system.time_zone, "+01:00/+02:00"; utc_offset() in base.c - names aren't portable); a change
                 -> setting system.tz_changed "<from>|<to>|<day>" -> a needs-a-look check until "Got it"
                 (system.tz_seen); a zone of "+00:00" (a Pi's default) is a Note with how to set it. Part 2
                 (0.15.0): the detection is the core's now (core/zone.c, at the start); the check says how
                 many days were kept, and the page offers what moving the typed times back would do
                 (previews + "Move them back to the times typed" = system.tz_retime, admins, once).
                 BACKUPS (backup.c, issue #35; owner, 2026-10-17: the secret ONLY in the download, the media
                 disk by default, 7 daily): tab /system/backups. Nightly (system_backup_due: none yet = 3 min
                 after the start, >26 h old, or >20 h old at 03:xx; a failure retries after 1 h) into
                 SET_BACKUP_DIR, else <first library folder on ANOTHER disk>/.hemma-backups (setting media.roots
                 read as text; disks compared as media/room.c does; the library scan skips dot folders), else
                 <data>/backups. Names hemma-YYYY-MM-DD-HHMM.db (the only files it ever deletes); the newest of
                 each of the last BACKUP_DAYS (7) days stay. One job at a time on a thread (JOB_LIST/BACKUP/STAGE
                 - a USB disk may spin up), results taken on the tick (g_done); under tests a job runs at once.
                 Settings system.backup_last "<unix>|<name>|<rows>|<bytes>", system.backup_fail "<unix>|<why>".
                 "Go back to this" stages it (store_stage_restore) and, under systemd/launchd
                 (system_restarts_itself), app_quit(HEMMA_EXIT_RESTART = 75) 2 s later (Restart=on-failure /
                 KeepAlive start it again); else "restart Hemma" + "Don't put it back". Download GET
                 /system/backups/<name>.tar (admins, a thread via conn_detach, 2 at a time): the backup +
                 hemma-secret.txt. Checks: Backups (OK / BAD failed / WARN >2 days / INFO same disk), "Going
                 back to a backup" while one waits, "Backup put back" (SETTING_RESTORED).
                 UPDATES (update.c, issue #73; owner, 2026-10-19: "a one-off command you can run!" + the issue's
                 opt-in check): tab /system/updates. Hemma never writes its own program: an admin's "check" /
                 "install X.Y.Z" (only the newer version the helper named) / an uploaded download ("install-file":
                 form[data-upload="/system/update"] -> POST plan (one .tar.gz, <= 64 MB) + one streamed PUT ->
                 <data>/update/upload.tar.gz, gzip magic checked, no resuming) goes as "<what> <16 hex nonce>" into
                 <data>/update/request (file_write_atomic) -> hemma-update.path -> the root helper, which answers
                 in /var/lib/hemma-update (ANSWERS; read here every 2 s, never written): latest "<unix> <ver>" |
                 "<unix> - <why>", result "<state> <unix> <ver|-> <words>" (fetching, installing, installed, back,
                 refused, broken). Present = both folders exist (else the tab says how to update by hand: plat_bundle()
                 names the download). Settings system.update_check ("1" = a check every 24 h from the tick; OFF by
                 default - nothing leaves the home unasked), system.update_asked "<unix> <what>", system.update_seen
                 (Got it). One install at a time (installing(): unanswered for INSTALL_SILENT_S 900 s, or fetching/
                 installing); no answer = WARN "the update helper didn't answer" (120 s for a check). Checks:
                 Update (installed OK / back, refused, broken WARN until Got it), Updates (out = INFO), and
                 "Operating system updates" INFO while Debian's unattended-upgrades isn't on (/etc/apt/apt.conf.d/
                 20auto-upgrades; the card shows the stamp's day; never read under tests). Tests:
                 system_test_update_dirs(ask, answers) after th_boot, system_test_update_os.
                 dependencies: tv needs media (it links modules/media/library.c), media needs av (the converter)
  media/upload.c admins add videos from a browser (plan + streamed PUTs, .hemma-part files, resume); a folder
                 holding x.mkv + x.web.mp4 sends only the copy (checkbox "also send originals").
                 AUDIOBOOKS (1.6): the plan takes .m4b .m4a .mp3 (media_is_audio_ext) as they are - never converted, never
                 checked against the library (media_have), never a video's companion (skip_companions) - and refuses the
                 rest as "not a video, subtitle, poster or audiobook". The preview reads "An audiobook" / "Part of an
                 audiobook" (UpSorted.part) / "Goes with <show or film>" (UPK_AUDIO) and counts "N audiobook files" / "N
                 audio files"; the card names one by its file (row_label); the picker's accept= adds .m4b,.m4a,.mp3,audio/*.
                 SORTING UPK_BOOK (step 1b, after the videos): an .m4a/.mp3 inside a show's or film's mapped folder
                 (map_of(..., false): a theme.mp3, Soundtrack/) is UPK_AUDIO instead and goes along with that folder in
                 step 3, like its poster; an .m4b is always a book. <books folder> = the library folder's own one
                 (media_is_books_folder, the first by name; "" when it is one), else "Audiobooks"; a book's file goes to
                 <books folder>/<its path below the deepest books folder among its folders, else all of it>; ?series= never
                 takes it. Its own folder (above CD1/, Disc 2/: media_disc_folder) maps there as a DirMap.book - only
                 pictures and subtitles follow it (map_of(..., books)), none made inside a show's or film's folder; pictures
                 named after the file go with it (step 2). Other MKVs
                 (box "make videos play everywhere on this device first", on by default) are converted BY THE
                 UPLOADING BROWSER (todo {i,f}; web/conv.js runs av.wasm in Workers; the finished copy STAYS IN ITS WORKER, which PUTs it
                 itself ({put, at} -> {status, body} | {lost}) and is terminated once sent - a copy handed to the page lived until
                 the page's next garbage collection: 28 GB of them on a PC's disk once, 2026-10-19; jobs start while the originals of
                 those running + waiting fit the form's "room" slider, 2-32 GB, pref media.upload_room) and only x.web.mp4 is
                 sent: POST <batch>/<i>?pm= (progress) / ?size= (copy ready -> PUT) / ?original=1&why= (the
                 converter refused it: the original goes under its own name; the card then says so out loud,
                 .media-up-asis, with the first reason) / ?sub=1&lang=&name=&forced= + SRT body (a text subtitle
                 track av_convert took out, conv.js {size, subs}: wire.js posts each after the copy's PUT ->
                 convert_save_sub next to the copy; never over a file). The box (and data-convert/
                 data-wasm on the form) only appears when av.wasm is built in. Resume trusts a ".hemma-part" only
                 if it's no longer than the file and not a picture (resume_point); one file is in one
                 upload at a time (claimed); Stop stops the upload whose card it's on. ONE UPLOAD AT A
                 TIME (owner's rule): while one is running (workers, or its browser active in the last
                 30 s - Batch.busy), a new plan is refused with who/how far, unless "force" ("Upload
                 anyway", shown only then) - both run, the running one's files skipped. ALREADY HAVE IT: a video
                 the library has under another name (library.c media_have: named as it will be there; same
                 show+season+every episode of its range+part (1.4: "Dark Side Pt. 2" is new beside "Dark Side"),
                 or same film title+year+part via names.c media_name_part "CD2") is skipped in the plan, before any converting, and so are its
                 subtitles/pictures (skip_companions); box "dupes" sends them anyway. Disc-rip titles numbered
                 only by order, extras and the file's own original/copy are never matched. A disc's
                 "play all" (spotted by size) sends just its first MB as x.playall.mkv: the header's duration
                 is what lets library.c tell featurettes from episodes without the play-all. The same
                 plan/PUT path sets pictures: {"art":"<series|film id>"} + one JPEG/PNG/WebP (magic-checked,
                 25 MB max) saved as <show folder>/poster.<ext> or <film folder>/<film name>.<ext> - where
                 the scan looks first (library.c media_art_target/_current/_prune); the page dialog is ?pic=1.
                 PICTURE SUBTITLES (1.2) from the browser's converter: PUT <batch>/<i>?pic=pgs|vobsub&lang=&name=
                 &forced=&t=[&idx=N] (the .sup, or the .idx text's N bytes then the .sub), streamed by pic_worker
                 (counted in MAX_WORKERS) to .hemma-part, magic-checked ("PG" / 00 00 01 BA), at most
                 AV_CONV_PIC_BYTES (413), .idx at most 1 MB, room as for any PUT; VobSub .sub then .idx (both or
                 neither), never over a file ({"skip":true}), only for a copy the browser made (originals: the
                 server's subtake job). g_subfiles (under g_lock) numbers text and picture tracks together by their
                 place `t` in the file (as convert_save_sub does) and answers a resend the same; text POSTs send t=
                 too. Subtitles never stop an upload. Dropped sidecars: .sup and .idx, and a .sub only with its .idx
                 in the same upload (upload_sort step 4: wherever its .idx goes; vobsub_pairs both or neither; the
                 .sub first in the todo); the picker's accept= lists .sup .idx .sub.
                 SUBTITLES ONLY (1.3; owner, 2026-10-23: "still keep the logic of being able to extract it from the
                 video"): an MKV/WebM that stays behind - a dupe (media_have_file: the library's video in full in
                 UpFile.target + how many subtitle files it has, lib_subs), an original whose copy goes instead
                 (skip_originals) or whose copy is on the server (convert_here: subs_beside counts its sidecars) -
                 becomes todo {"i","f","subs":1} (subtitles_only, needs conv) when the library's video has none
                 and no subtitle file of the batch goes along (UpFile.sidecars): the browser reads only its tracks
                 (av.wasm conv_subs_only = av_subtitles), posts ?sub= / PUTs ?pic= as for a copy (copy_of -> the
                 target) with ?pm= progress (shown on the card, "its subtitles"), then ?pm=1000 = done (subs_due:
                 the batch stays open until then; counted as a file of the upload). Preview fate FATE_SUBS "Only its
                 subtitles". SIDECARS FIND THEIR VIDEO: rehome() renames a subtitle file of a dupe (skip_companions)
                 to "<the library video's stem><its own tags>" in the library video's folder ("goes next to X, which
                 the library has"; one there already = skipped "X has these subtitles already"); lone_subtitles: a
                 subtitle file with no video in the batch tries its name's stems SHORTEST first (media_sub_for takes
                 only a rest of tags, so every tag stays) as "<stem>.mkv" with media_have_file.
                 SENDING ORDER (1.3; owner: "be always at that limit"): wire.js sends ONE file at a time and never
                 waits while something can go - what the converters finished first (converter().take(), in the
                 order they finished), then the plain entries in the plan's order, else converter().next(). No more
                 "a few ahead of the one being sent": the room slider alone bounds what waits. IN MEMORY (1.3;
                 owner: "the option of storing converted videos in RAM or on a drive ... as a big checkbox. If
                 other people use it, the old style is nice"): the form's "ram" box (in sight under the preview,
                 pref PREF_UPLOAD_RAM media.upload_ram, saved with a converting plan, off by default) -> conv.js
                 keeps the copy in 16 MB parts of plain memory (else a Blob, which the browser keeps on its disk
                 past its own limit; out of memory mid-copy -> the rest goes on in a Blob) - at most RAM_MAX (8 GB)
                 of copies in memory per tab (wire.js held()/inRam: past it a job's copy is a Blob; all a tab's
                 workers share ONE process, and at ~12-16 GB its reads failed) - and PUTs it a part at a
                 time: PUT ?at=A&end=E (Job.piece: saved, no rename, answered {"ok":true,"at":E}; the last piece's
                 answer is a whole file's). form[data-part] (the browser tests only) = smaller parts.
                 READS THAT FAIL (1.3; 13 episodes went up unconverted: "Failed to execute 'readAsArrayBuffer' on
                 'FileReaderSync'" while the tab held ~12-16 GB, though the files read fine for the PUT): conv.js reads
                 once more in a 1 MB piece, then posts {error, again} (a failed read, or "memory"/"allocat" in the
                 message); wire.js converter() puts such a job back in line (again[], first in pump(), AGAIN = 2 times
                 at most) instead of sending the original. form[data-flaky="N"] (tests only): each job's first try
                 fails N reads (upload.spec.mjs "tried again").
                 A DROPPED NETWORK (1.4; 2026-10-27: a PC's Wi-Fi gone ~10 min at 4 am stopped a 425-episode upload
                 for five hours - "The connection keeps dropping" after a minute of tries): wire.js down(since) - a
                 request that got NO answer (fetch rejected; conv.js send {status: 0}) isn't a try: it's asked again
                 every netPause (5 s; form[data-netpause] in tests) for up to NET_WAIT (30 min), put() then GETs where
                 the file got to. Only the server's own answers count as tries (put 8, ask 5, subtitles 3-4).
                 upload.spec.mjs "waits out a dropped network" aborts 12 PUTs with internetdisconnected.
                 THE CARD (1.5; owner, 2026-10-27: "it sort of spams between texts ... make it more modern", "a list of
                 the most processed items going up, and make it say if it's processing / uploading"): render_batch = a
                 state pill (Processing / Uploading / Waiting for the browser - 30 s without a sign - / Finished /
                 Stopped) + the count ("Uploading · 3 of 9 files" read as one heading: an sr-only " · "), one bar
                 (batch_permille), then dl.media-upc-stats Sent / Speed / Time left in fixed places ("–" when none):
                 Batch.rate is measured while bytes come (Batch.wrote) - also across a memory copy's pieces, each its
                 own PUT, which used to restart it - and shown UP_SPEED_KEEP_S (60) after the last byte; time left =
                 with copies still to make, the pace of the last UP_PACE (8) saves (UpFile.touched), else bytes/rate,
                 coarser the longer. Then the rows under way (row_kind), the furthest along on top, ROW_CAP each:
                 Saved (the 2 newest, HH:MM), Uploading (UP_WRITING, or its bytes came within UP_GAP_S 30 s - between
                 pieces the old card's "Now:" line came and went every second or two), Ready (CV_READY, or CV_WAIT at
                 pm 1000 = made, waiting its turn: wire.js finish() reports ?pm=1000; a later, lower report never
                 takes it back), Processing (CV_WAIT, pm > 0), Subtitles (a subtitles-only entry being read; at 1000 it
                 stays done: a progress report arriving late can't open it again), then "N more waiting". Rows
                 are li#media-upc-<batch>-<i>, named as Hemma reads the sending device's name (row_label: "Show ·
                 S04E12 · Title", "Film (2003) · part 2"; a name that says no more shows its folder below instead).
                 PROCESSING SPEED ("9% · 2 MB/s"; owner: "no like indication on if it's actually doing something" - pm
                 follows the video's time, and copying reads the whole file before 30%): conv.js counts what the
                 converter took in (fed, the bytes hemma.read handed it) and posts {pm, fed} with progress and every 2 s
                 while reading; wire.js sends ?pm=&fed= (tell/beat keep the last fed); pace_note measures over
                 PACE_WINDOW_MS (4 s) or more, a window with nothing taken in = 0 at once (the heartbeat repeats the
                 count every 10 s), shown while reports come (PACE_SHOW_MS 30 s). upload_test_pace_window() in tests.
                 The tick's signature counts the pm (and the pace) of what's being made, so those rows move too.
                 THE PAGE (1.1; owner, 2026-10-21: "easy enough to understand"; "Add videos" in Watch's header,
                 "Add episodes" on a series page): form[data-upload][data-preview] with a [data-drop] area (wire.js
                 keeps dropped/picked files and folders on the form, each choice adding; [data-drop-clear] empties),
                 then the server's PREVIEW: live action media.upload_preview ("files" = [[path,size]] JSON cut at
                 ~900 KB, "all" = the count; kept in 8 slots by the screen's live "media.up"; upload_test_preview()
                 in tests) = plan_make() dry (nothing on the disk changes), grouped by destination folder, each file
                 what Hemma thinks it is + its fate; the series found get rename fields "to.<16 hex of
                 upload_show_key(found)>"; Start = the same plan (PlanIn read alike from JSON and fields). More
                 options in a <details>. ?series=<hex>: everything into that series' folder. "Upload anyway" is
                 [hidden] unless one is running (media.css: .media-up-form [hidden] beats .check's display).
                 SORTING (upload_sort.c; hidden sort=1 - keep=1 or no sort = the chosen layout under into): episode
                 -> <Series|Serier|TV Shows|TV|Anime, else Series; '' when the library folder is one>/<the library's
                 folder for that show in this library folder, else its name>/<its season folder (not a disc's),
                 else Season N|Specials>/<name>; disc rips keep their path below their show folder; film ->
                 <Movies|Films|Filmer>/<Title (Year)> or the film's folder; subtitles/pictures/copies go with their
                 video; what sits in a show's or film's own folder goes along; the rest (and episodes of no named
                 series) as chosen. File names never change. upload_safe_name: ':' -> ' -', no leading dots. THE SHOW'S FOLDER (show_home, 1.2.0): of each of its episodes' folders the shallowest that names the show (media_names_show), the one most episodes share (tie: the shorter); else the first episode's show folder unless that's the library folder itself or a Series folder; else a new <series folder>/<Show> - a copy at the top level once sent new seasons there.
  media/download.c + zip.c  DOWNLOADS (1.1; owner, 2026-10-21: grown-ups only - members and admins, never
                 children, guests or display accounts, media_grown_up; the version that plays everywhere; a season
                 or a series as ONE .zip): GET|HEAD /media/file/<id>?download=1 = the playable file +
                 Content-Disposition (media_download_name: "Film (2020).mp4", "Show S01E03-E04 - Title.mp4", else
                 its own name; media_safe_name drops \/:*?"<>| and Windows' device names; filename= ASCII +
                 filename*=UTF-8''), Range as ever. /media/zip/<series>[?s=N] (stream.c zip_thread; MEDIA_MAX_ZIPS
                 2, else 503 + Retry-After: 60) = "<Show>/Season N|Specials/<episode name>" per MAIN episode's
                 playable file + its subtitles (".sv.forced.srt" tags kept), names unique ("(2)"), STORED + data
                 descriptors, UTF-8 names, DOS times from the mtimes, ZIP64 only where a number needs it,
                 Accept-Ranges: none; folder/zip names stay English (media software and our parser read them).
                 zip.c is pure: zip_size = Content-Length to the byte (the thread stats the files again first),
                 zip_write driven by read/write callbacks. Buttons (never in TV mode): media_download_link at the
                 end of the watch page's .media-playable row, media_download_series = .toolbar#media-dl under the
                 season tabs (a one-season series: "Download all" only; sizes media_size_text, a series' =
                 media_zip_bytes). GIVING WAY (a Pi 3B+'s disk and network share one USB 2 bus): while an
                 ordinary video stream runs (g_watching; downloads counted apart, g_downloads) each download sends
                 at most MEDIA_DOWNLOAD_SLOW (4 MB/s) - pace(), re-asked every second; full speed when nobody
                 watches; DOWNLOAD_STALL_MS 5 min (a zip can't resume). Logged ("media: eva downloads ...").
                 Tests: media_zip_test_busy, media_download_test_pace; download_test.c holds a little unzip (end
                 records, central directory, local headers, descriptors, CRCs).
  media/manage.c LOOKING AFTER THE LIBRARY (1.1; owner, 2026-10-21: "easier and better control of the stuff
                 inside"), admins only: the series page's Manage (/media/series/<id>?manage=1[&s=N]; top to bottom - owner,
                 2026-10-25: nothing to scroll past - "The whole series" card (Rename, Remove the series), the season
                 tabs, the season's card with its Remove first; per season
                 each episode/extra with its files (Original / Converted copy badges) and size, "Fix number"
                 (&fix=<id>), Remove (&rm=<id>); Remove season (&rm=season), Remove series (&rm=series)); the watch
                 page's "Fix title" (?fix=1; films and other videos - a film without a year shows up as one);
                 Library settings' "What takes room" (10 biggest series/films of 1 MB or more, originals + copies).
                 REMOVING: remove.c media_purge_episode/_season/_series plan from the index (no disk work: the
                 dialog); media_purge_job + media_purge_run delete on a thread (one at a time, g_removing; tests
                 run it in the action): the item's files, then its OWN folders whole (a season's "Season N"; a show
                 folder that names the show, media_names_show) when the walk finds nothing else - a video neither
                 the item's nor an extra by name, a .hemma-part/.conv.part, a link, > PURGE_MAX 5000 entries,
                 another library folder inside = kept - then empty folders up to (never) a top-level one.
                 media_manage_tick: the gone videos' watch history goes, Latest "removed %s from the library", a
                 toast to the asker, rescan. Refused while converting. FIXING renames on disk: a film -> "<Title>
                 (<Year>)<ext>" (+ .web.mp4 copy, subtitles/picture sharing its stem, its own folder as
                 media_remove_plan finds it); an episode -> "<Show> S01E03[-E04][ - title]<ext>" moved into the
                 show's season folder (one there for that number, else "Season N"/"Specials"; only when the show
                 has a folder of its own). clean_part + upload_clean_rel for names; the new name is parsed first as
                 the library will ("Hemma would read that name as ..."); never over a file (rename_safe; a
                 case-only change goes through a temp name); every move or none. Watch history moves to the new id
                 at once (media_watch_move; the rescan's watch_follow fills the rest); "Not for children" marks of
                 a renamed folder follow (kids_follow), an episode leaving a marked folder marks its new one.
                 RENAMING A SERIES (1.3; owner, 2026-10-24: "make it so the admin can rename the series", Manage ->
                 "The whole series" -> Rename, ?rename=1, action media.rename_series id/title/year): its SHOW FOLDERS
                 are renamed (show_dirs: each folder above its videos' season/disc folders that media_names_show the
                 series - every one of them, up to 16; refused when a video lies in none (named by its file, or in
                 the library folder), another video of the library is in one, one is inside another), "<title>" or
                 "<title> (<year>)", all or none (Moves). First the bare folder name is parsed (".../Season 1/S01E01.mkv":
                 "Show S01-S03" or "Season 2" refused - "Hemma would read that name as"), then every video of it at
                 its new place must parse to that title (a file not in a season folder keeps its own name's show:
                 "use Fix number on it first"); another series of that title, a conversion of it, an upload into the
                 folder (upload_claims_under) or a removal running = refused. THE ID STAYS: a series' id is its folded
                 title hashed (library.c series_id), so table media_series_name {title folded, series} - written by
                 media_series_keep(id, new title): its old row and any row of that title go, none when the title
                 hashes to the id itself - gives the new title the old id; the scan reads a copy (Scan.names, a
                 thread-local t_names around index_finish), the event loop the table. TV channels (Channel.shows),
                 screen rows, media.sub.<series> picks and bookmarks keep working unchanged. Watch history moves per
                 video (media_watch_move to media_make_id of the new path), "Not for children" marks follow
                 (kids_follow), Latest "renamed %s to %s".
                 Numbering one ripped disc's title by hand makes library.c number the rest after the season's
                 highest (the fix dialog says so).
  media/convert.c "play everywhere": MKV -> <name>.web.mp4 next to the original (H.264 + AAC, faststart),
                 by av/conv.c (the same engine browsers run): MPEG-2 decoded, inverse-telecined/deinterlaced
                 (av/deint, chosen by sampling 4 windows) and re-encoded; H.264 copied; HEVC copied as 'hvc1'
                 (hvcC arrays marked complete - what Safari needs; no HEVC decoder here, so it plays on Apple
                 devices and browsers with HEVC hardware, not everywhere; Convert says "play in Safari too", the watch page
                 badge "Plays on newer devices"); AV1 copied as 'av01' (2.1; owner, 2026-10-29: an AV1 upload went up as
                 it was - "make this viable also as a convert"): av1C = MKV's CodecPrivate (the AV1CodecConfigurationRecord;
                 none = refused, saying so), a temporal delimiter leading a block dropped, the 'av01' brand in ftyp; no AV1
                 decoder (a big one, and slow), so it plays in Chrome/Edge/Firefox, Android and most newer TVs, and in
                 Safari only on Apple's AV1 devices (iPhone 15 Pro+, M3+) - the badge "Plays on newer devices" as HEVC's,
                 the player's trouble line its own, Convert "Make it play on more devices" (or "Fix the sound"). Checked on a
                 real 10-bit 1080p AV1 + AC-3 film (2 h 28 min): every packet as the original's, ffmpeg decodes it, Chrome
                 plays it;
                 EVERY sound track it can convert goes into the copy (2.2, up to 8 - "Where things stand" 2.2.0; before, only
                 the default one); AC-3 / E-AC-3 (Dolby Digital Plus) and DTS (1.3: the core, 5.1 -> Lo/Ro) -> AAC at the track's own rate (48, 44.1 or 32 kHz - TV rips are 44.1; 1.1.2; E-AC-3 half rates refused; a frame at another rate than the file says = unsupported); AAC copied; MP3 copied (2.5: MP4_MP3, mp4a + esds object type 0x6B, or 0x69 below 32 kHz; mp3_frame_size splits a block into its frames, 1152 or 576 samples each); Opus copied (2.5: MP4_OPUS = 'Opus' + dOps made of the OpusHead by opus_dops - version 0, big-endian, mapping families 0/1 only, else the track is left out; each packet a sample of opus_samples(TOC) at 48 kHz, put_audio_d with its own length, rdur[] in runs; the pre-skip = the edit list's start; an MKV, not a WebM, with Opus is a remux; an MP4 with it plays as it is; mkv_mp4.h turns a dOps back into an OpusHead; checked on an ffmpeg-made H.264 + Opus MKV: ffmpeg decodes the copy cleanly, every packet at its time, Chrome plays it); the file's subtitles next to the ORIGINAL - also
                 after an `unsupported` refusal (1.2) - by convert_save_subs/convert_save_sub(video, AvConvSub,
                 ConvertSubs*): text .srt, PGS .sup, VobSub .sub + .idx (the pair or nothing, .idx last), media/subs.c
                 names ("x.en.srt", "x.sv.forced.srt", "x.en.sdh.srt", "x.en.2.sup"); a second track of the same
                 language and flags in one file = ".2" (counted by the file's tracks, kinds together, saved or not);
                 never over a file. The library gives x.*.srt to x.web.mp4 once x.mkv is gone; the watch page's
                 card for HEVC + Dolby says "Fix the sound"). convert.c = files + the queue: one file per worker thread
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
  media/subs.c   SUBTITLES (1.2; owner, 2026-10-23: "all the shows that have subs to be able to give subs"): SRT ->
                 WebVTT (/media/sub/<id>.vtt) and which file belongs to which video. media_sub_tags(video, sub,
                 &MediaSubTags{lang, sdh, forced, n}) reads a name: sdh/cc/hi = SDH, forced/foreign = forced; a
                 number counts only after a language ("x.en.2.srt" yes, "Movie 2.srt" never); "und" = a language
                 that names none. media_sub_for = the language only. media_sub_key = "en", "en.sdh", "en.forced",
                 "en.sdh.forced", "en.2", "und". media_sub_name / media_sub_sidecar(video, lang, name, forced, ext,
                 n) = the names on disk; a numbered one with no language is "x.und.2.srt". media_is_sub_ext = .srt
                 .vtt .sup .idx; media_is_sub_pic = .sup .idx .sub; media_is_vobsub_data = .sub. THE .sub
                 COMPANION: library.c indexes a .sub as MF_SUB only beside an .idx of the same name
                 (vobsub_idx_beside, a stat at the scan), linked to the .idx's video - so remove (media_remove_plan,
                 media_purge_*), Fix title/number (manage.c companions) and zips (download.c: an .idx only with its
                 .sub, right after it, one name) carry the pair; a lone .sub (MicroDVD text) isn't indexed and is
                 never offered. media.c sub_choices() = the offered subtitles, the <track id="sub-<key>">s and the
                 menu in one order: by language (unknown last), plain, numbered, SDH, forced; keys unique (a
                 Hemma-named file keeps its key, the other of two alike gets the next free number); labels = the
                 language in itself + "(for the hard of hearing)" / "(foreign speech only)" / "English 2";
                 media.sublang turns on the first in that language (the plain one; else its first picture track).
                 FOLLOWING THE VIEWER (1.3; owner, 2026-10-24: "if enabled on a episode, to carry on to the next
                 episode with the same language if it exists"): the player reports the viewer's pick (the menu, or
                 C) with the live action media.sub (id, key; "" = off; .display - the TV's viewer -, ACT_ALL) ->
                 pref "media.sub.<series hex>" = "<generation> <key|->" (episodes only; films keep nothing).
                 sub_choices: that pick wins over media.sublang - the same key, else the first of its language
                 (menu order, as wire.js picks), else none; "-" = off. media.prefs (a new "Subtitles on by
                 default") bumps "media.sub.gen": every series starts from the language again. UiSub.pref ->
                 data-subpref marks what C turns on (that pick, else the language's).
  media/subtake.c TAKING SUBTITLES OUT BY THEMSELVES (1.2): a thread (one, background priority, tick-started, never
                 under tests) reads each library MKV/WebM once (not play-alls, not .web.mp4), newest first: mkv_open
                 first - no track AV_CONV_CODECS takes, or every sidecar already there = done after the headers;
                 else av_subtitles + convert_save_subs next to it. Waits (a half-read file is read again from its
                 start): setting media.take_subs "0" (on by default; admins: Library settings card "Subtitles inside
                 videos" with how far it's got), media_lib_away(), convert_busy(), media_watching_active() (+60 s
                 quiet), the first 60 s after a start. A file that isn't readable Matroska = done; one that vanished
                 (an unplugged disk) isn't. Done = raw table media_subtake(rel PK - below the library folder, so a
                 disk mounted elsewhere isn't read again -, size, mtime, tracks, saved, at), format
                 media.subtake_format (a new one reads everything again); the library rescans at a run's end and
                 every 5 min while one goes. Tests: subtake_test_step() (1 looked at / 0 none left / -1 waiting),
                 _reads/_opens, _halt_after(n).
                 PICTURE SUBTITLES on the wire (stream.c): GET /media/subpic/<id>.json {"w","h","cues":[[start,end,x,
                 y,w,h]..]} and /media/subpic/<id>/<n>.png (subpic_render + png_indexed) for a library .sup / .idx
                 (+ its .sub) the viewer's kids rules allow, else 404; on threads (4 at once, else 503),
                 Cache-Control private 1 day; MEDIA_SUBPIC_CACHE (4) opened files by id+size+mtime (the files opened
                 only while read). sub_choices offers .sup/.idx in the one list, UiSub.pic = "/media/subpic/<id>.json
                 ?v=<size>-<mtime>&k=<key>", no <track>. PNG addresses = the JSON's with ".json" -> "/<n>.png", the
                 query kept (they ride on the cookie: a key fits its exact path only). Scale from the JSON's w/h,
                 never the video's size: MakeMKV's .idx says 720x576 for an NTSC 720x480 disc.
  media/cats.c   CATEGORIES (1.4; owner, 2026-10-27: "increase the searching and usage of the series and movies"; "by hand +
                 .nfo genres", ticked by "admins and members"): a WORK = a series (key "s<16 hex>" = its id, kept through a
                 rename) or a film ("m<16 hex>" = media_film_key: folded title + "|" + year hashed - its parts and copies,
                 moved or not, share it; Fix title moves its rows: media_cats_move). Its categories (media_cats_of, at most
                 MEDIA_CAT_MAX, listed starters first in their order, then the household's A-Z) = the genres of its .nfo
                 (library.c MF_NFO: a series' tvshow.nfo in its show folder, else the first episode's; a film's
                 "<stem>.nfo", else movie.nfo when alone in its folder; an episode's own .nfo is never indexed - read on
                 the scan's thread, once per size+mtime, kept in name.title '|'-joined and in the cache's title column)
                 and of the lookup (media_lookup_genres), mapped onto STARTERS (key, N_ name, `also` = other names folded:
                 en/sv/es and the services' genres - "Animation", "Children", "Science-Fiction", "Sitcom", "Komedi"; one
                 may map to two: "Action & Adventure"), else "+<name>" (the household's own; the first spelling in
                 use wins, folded), plus table media_cat {work, cat, on, at}: on = ticked, on = 0 = one the .nfo/lookup
                 gave taken off (a row only where it differs from them). A category = a starter's key or "+<name>"
                 (media_cat_parse for ?cat=). Actions media.cat (work, cat, on) and media.cat_new (work, name: a starter's
                 name in any language = that starter) - members and admins (no .may: children, guests, display accounts
                 never; links to the dialog ask ui_can). media_cats_line on a series' / film's page (chips -> the list
                 ?cat=, + "Categories" -> ?cats=1), media_cats_modal (?cats=1: a toggle form per starter and per own
                 category in use, "A category of your own", admins: media_lookup_form). media_cats_used(c, kinds) =
                 categories with counts in media_lib_for(c). USED BY media.c: Movies/Series chips (ListChoice: cat, sort
                 "" | new | year (films) | watched (viewer), show "" | new (not started) | going (started, not finished)
                 - list_href keeps the three), Watch's home rows (CAT_ROWS 6, two titles or more, the fullest first, See
                 all = /media?cat=X: category_page), search (media_cats_words: the reader's label + English); tiles.c
                 block media.category (choices = the categories in use) - screens heads a picked row with its choice's
                 label (screens_page.c draw_rows: "Comedy", "Family Guy", not "A category"/"A series").
                 SEARCH (media.c, 1.4): query_parse = folded words (word chars: ASCII alnum + any UTF-8 byte), one episode
                 number token ("s02e03", "2x03"); EVERY word somewhere in the item's text (series: title year categories;
                 films: + rel; episodes: show + episode title, and only when a word reaches the episode's own title or
                 the number was asked - "simpsons" never lists 683 episodes; others: title + rel); title_rank 4 whole, 3
                 start, 2 word starts, 1 in it, 0 elsewhere; groups Series 24, Movies 24, Episodes 30, Other videos 12
                 ("and N more").
  media/lookup.c CATEGORIES LOOKED UP ONLINE (1.4; owner: "if internet is available ... if the user has checked the box",
                 "It can be grayed", "press manage, and then maybe 'Update tags'? Where i paste the id"): setting
                 media.lookup (OFF by default; admins, Library settings card "Categories from the internet"), media.omdb_key
                 (letters and digits; settings are api_hidden). The INTERNET CHECK: the card asks (g_probe_wanted, at
                 most every PROBE_EVERY_S 300 s), the tick sends GET http://api.tvmaze.com/ on a thread; no answer =
                 g_online -1 -> the box disabled (unless on) and "can't reach the internet", media.lookup on refused;
                 a lookup without an answer marks it too; while on and unreachable the check repeats every
                 OFFLINE_WAIT_S 900 s. LOOKUPS: one thread at a time, ASK_EVERY_MS 3000 apart (TVmaze allows 20/10 s);
                 next_job = series without a row (by name: TVmaze /search/shows?q= - the one premiered in its year,
                 else of its very name, else the first; genres + type Animation/Documentary), then films (OMDb
                 /?type=movie&t=&y=&apikey=, only with a key OMDb hasn't refused - Response False "...API key" = -2,
                 g_omdb_bad on the card; OMDb answers a bad key with a 401 + JSON). Table media_lookup {work, genres,
                 source, ref, name ("That '70s Show (1998)": a wrong match shows), found, pinned, at}: found = false is
                 asked again after NOT_FOUND_DAYS 30; an answer not understood (or a 5xx) is stored as not found one day
                 before that (asked again tomorrow, never every 3 s). BY AN ID (media_lookup_form: Manage's "The whole
                 series" card and the categories dialog, admins; action media.lookup_id work id): a TVmaze show link or
                 number -> /shows/N, an IMDb tt-id (any link holding one) -> TVmaze /lookup/shows?imdb= (301 to an https
                 /shows/N: its number asked again over http) or OMDb /?i=; an empty id = by name again; g_ask goes next,
                 the result is pinned (never re-asked by name) and the admin gets a toast (app_notify_person). A series
                 rename / a film's Fix title: media_lookup_renamed (by name = asked again; pinned moves). Lists re-drawn
                 at most every 15 s while answers come. Under tests nothing is fetched: media_lookup_test_tvmaze/_omdb/
                 _next/_online/_put (cats_test.c). A RENAME HOLDS the asking by name (g_hold) until the library has read
                 it again (media_lib_gen changed, no scan running or queued): Manage's Rename looked DuckTales up in the
                 same second with the old name and no year and matched the 2017 show (2.5).
                 PICTURES (2.5; owner, 2026-10-30: "It should automatically fetch if an image is not given", no new key):
                 setting media.lookup_art (OFF by default; card "Pictures from the internet" under the categories card,
                 greyed like it; media_lookup_card draws both). A series or film whose poster is 0 gets one: a series'
                 from TVmaze (image.original, else medium, fetched over plain http from static.tvmaze.com - in the same
                 job as its genres when both boxes are on: Job.both; else an art job by name, or /shows/<ref> when the
                 categories row was pinned by an admin), a film's from Wikipedia over https (fetch_https - needs curl;
                 none = no film jobs and the card says so): en.wikipedia.org/w/api.php generator=search "<title> <year>
                 film" + prop=pageimages|description piprop=original pilicense=any (non-free posters) - wiki_answer
                 takes the first in the search's order with a JPEG/PNG/WebP picture that is a film ("film" in its
                 description or its title's brackets) of its year (description or brackets), or without a year whose
                 title minus brackets is ours. Saved on the THREAD by save_picture to media_art_target's place
                 (art_dest: media_art_folder + "/" + stem, + .jpg/.png/.webp by its magic) only when
                 media_art_on_disk finds no <stem>.<image ext> there (-4: a picture came meanwhile - the household's)
                 - so the scan, other players and Set picture all see it as any picture. Table media_art {work, got 1
                 saved / 0 none found (asked again after NOT_FOUND_DAYS) / -1 couldn't (a day), pinned, source, ref,
                 name, size, at}; renames move a saved or pinned row, drop the rest. Saved pictures -> a rescan 10 s
                 after the last (ART_QUIET_MS), at most 60 s after the first. HEMMA'S OWN = the work's poster file has
                 the size saved (hemmas_own): media.art_id (work, id; admins; the form under the categories one in
                 media_lookup_form - which shows only the pictures' part while categories are off) replaces only that
                 one (Job.replace: removed on the thread if still that size, then the new one saved), never a picture of
                 the household's own (refused). Ids: series a TVmaze link/number or an IMDb tt (as categories); films
                 a Wikipedia link (any language: <lang>.wikipedia.org/wiki/<page>, titles=<page>&redirects=1). Tests:
                 media_lookup_test_tvmaze_pic/_wiki/_job/_save/_art_put (cats_test.c pictures_*); a real run on a PC
                 fetched DuckTales (1987) and five Disney/Marvel films' posters correctly.
  media/devices.c WHAT YOUR DEVICES PLAY (2.5; owner, 2026-10-30: "check the issue with the videos that are not playable
                 everywhere and how we can make them playable everywhere" - "Play everywhere 1456 of 2776", the rest
                 HEVC/AV1 copies; "Both, devices first": measure the household's devices before any HEVC -> H.264 work;
                 "Just the number": nothing had failed). media_caps(b) = <i hidden id=media-caps data-caps="type|type...">
                 on Watch's head (media_lib_head) and every watch page; wire.js capsTell() answers it ONCE A LOAD (after
                 settle and on the socket's open) with canPlayType per type - y probably / m maybe / n - as the live
                 action media.caps (live, .display, ACT_ALL: a TV's display account reports too). CAP_TYPE[] is append
                 only (CAPS_FORMAT + 1 for any other change: older answers stop counting): H.264 High 4.0, HEVC Main and
                 Main 10 at 5.1 (an HEVC file is matched against Main 10: x265 releases are 10-bit; the probe keeps no
                 bit depth), AV1 8/10-bit, AAC, MP3, Opus, FLAC, AC-3, E-AC-3 in MP4, WebM VP9+Opus, WebM AV1, Vorbis,
                 Matroska. Table media_device {person (the session's - a display account's own), agent (Session.agent,
                 packed), caps, format, first, seen}: one row per person + browser + answers; seen written at most
                 hourly; a device COUNTS while seen within DEVICE_DAYS (30), by someone present; rows go after 90 days
                 or with their person (media_devices_forget, in media's forget hook). cant(caps, probe) = the kind a
                 device can't play ("HEVC (x265)", "Dolby sound", "MKV", "unreadable files" for PLAY_UNKNOWN ...) or
                 NULL; media_device_plays. Library settings: Status gets "Play on your devices N of M" (videos = the
                 "Play everywhere" set: ix->recent's playable files); media_devices_card = the reasons the rest doesn't
                 play everywhere (probe.why counted, said through media_probe_why of one of them) for everyone, and -
                 admins - each device ("Anna · Chrome on Windows — plays N of M · not 1250 HEVC (x265)") + "N of M
                 videos play on every one of these K devices". The watch page (media_devices_line, for PLAY_MAYBE/NO):
                 "Plays on every device of yours" / "Doesn't play on: Living room TV (Chrome on Linux)". The tick logs
                 the breakdown when a scan changed it ("media: 1456 of 2776 videos play everywhere, 2700 on all 4
                 devices seen - the rest: ...") and each new device with what it plays - readable in the journal.
                 Tests: devices_test.c, tests/browser/devices.spec.mjs.
  media/kids.c   NOT FOR CHILDREN (issue #44; owner: "per folder"): table media_no_kids (root path + folder below
                 it, "" = all), marked on the Folders tab (media.kids, admins; Library settings lists them).
                 media_kid(c) = a child, or a display account with a child picked (ctx_viewer). media_lib_for(c)
                 = media_lib() for others, else a copy of the index with the movies/others/recent/extras/series
                 lists filtered (files, episodes, roots shared; remade when media_lib_gen() - bumped at every
                 swap - or the marks change): a series with ANY episode marked goes whole. By id: media_kids_ok /
                 media_kids_series_ok (watch, series, stream /media/file -> 404, Continue watching, browse, tv's
                 channels: a channel airing anything marked is left out for them - for_kid in tv.c). Audiobooks
                 (1.6): the copy's books list too - a book with ANY part marked goes whole (media_kids_book_ok).
  media/books.c + booklist.c AUDIOBOOKS (1.6; owner, 2026-10-28: "audio books library ... another tab or slot about
                 audio books, and also make it so that we can listen to it while the phone is locked"). WHICH FILES:
                 mediainfo.h media_is_book_file(root_name, rel) = an .m4b anywhere, an .mp3/.m4a only below a books
                 folder (media_is_books_folder: Audiobooks, Audio books, Ljudböcker, Audiolibros, Hörbücher ... any
                 case; or the library folder itself is one) - a show's theme.mp3 or a film's soundtrack never is.
                 They're MF_AUDIO files (library.c walk): add_file reads modules/av/audiotags.c once per size+mtime
                 (MediaTags in MediaIndex.tags, sorted by file id - NOT in every MediaFile: an index holds ~10,000
                 files) and fills the probe's container/duration/acodec; the cache keeps them in media_file's
                 `tags` (\x1f fields, TAGS_FORMAT first - a newer one, and the scan reads the files again) and
                 `chapters` (\x1e between) columns, added in place by ALTER like fp. BOOKS (booklist.c
                 media_books_build, end of index_finish, any thread): an .m4b is a book (several named as parts of
                 one title - media_name_part - one book); .mp3/.m4a: every one in a folder is one book, a short
                 CD1/Disc 2/Del 3 folder's with its parent's, by disc, track tag, then natural name order ("2"
                 before "10"); one loose in a books folder or the library folder = a book of its own. Title: an
                 .m4b's title tag > album > file name; parts: album > folder name; a tag that's a file's name (one
                 word with '_': LibriVox's "velveteen_rabbit") is skipped; "(Unabridged)" and which recording it is
                 at the end ("(version 3)", "v2", "ver 2", "(solo version 2)" - version_mark) dropped. Series =
                 the folder holding 2+ books (not a books folder, not the library folder), numbered by the number
                 their name starts with. Picture: "<stem>.jpg" beside an .m4b, else (alone in its folder / a folder
                 of parts) cover|folder|poster.*, else MediaBook.cover = a part with a picture INSIDE (served by
                 GET /media/cover/<file id>?v=: stream.c reads just those bytes, cover_off/len found by the scan).
                 MediaBook.id = its FIRST PART'S FILE ID and each part's MediaFile.series = its book's id - so a
                 book's progress is a plain media_watch row (pos/dur in the WHOLE book's seconds), and media.progress,
                 media.watched, media.forget work on it unchanged; media_watch_report marks an MF_AUDIO row done only
                 in its last 15 s (a video at 92%). media_in_progress (Continue watching) skips MF_AUDIO;
                 media_books_going is Continue listening. PAGES (books.c): /media/books = Watch's "Audiobooks" tab
                 (media_lib_head shows it only while media_lib_for(c)->nbooks; the search box searches books too:
                 media_search_shown; its placeholder "Search audiobooks…" there), Continue listening, then a section
                 per series, then ONE "Books" for the rest (the list is sorted by series-or-title, so the rest isn't
                 together in it); /media/book/<id>
                 (?at=<s>: start there, autoplay - a chapter's link without wire.js) = cover, author/narrator/year/
                 length, series line, the player, Mark as finished, Download (grown-ups: one file =
                 /media/file/<id>?download=1, parts = /media/zip/<book id>), "About this book" (the description
                 tag), the chapters (a part's own, else each part one - "01 - Chapter One.mp3" -> "Chapter One",
                 "01.mp3" -> "Part 1"; #media-chapters data-static, a[data-at]), "Next in the series". THE PLAYER's
                 markup (wire.js "the listener"): <audio id=media-audio data-book data-static data-id data-report=
                 media.progress data-length data-resume data-title data-author data-art [data-parts]> with the
                 browser's controls until wire.js hides them, and div[data-listen=media-audio][data-static] (hidden,
                 data-paused) holding [data-b] chapter seek at left play prev back fwd next speed speed-t sleep
                 sleep-t, [data-speed] and [data-sleep] menu items, .media-ab-err (data-err). Watch's home shows
                 Continue listening (media_books_shelf); a library of only books says so and links the tab.
  media/drives.c USB DISKS (issue #74): media_drives_scan(sys, udev, mountinfo) reads /sys/block/<dev> (dev, size,
                 partitions = subdirs with a "partition" file), udev's /run/udev/data/b<maj:min> (ID_FS_TYPE/UUID/
                 LABEL_ENC, ID_MODEL_ENC; USB = ID_BUS=usb or any ID_USB_DRIVER - a USB-SATA bridge says ata) and
                 /proc/self/mountinfo by maj:min (system = holds / /boot* /usr /var /home or swap) - no root needed,
                 every 5 s from the tick (never under tests unless media_test_drives). Offered: USB, not system,
                 nothing on it mounted (a disk set up by hand is none of Hemma's business): a usable volume (ext2-4
                 xfs btrfs exfat vfat ntfs + a UUID) -> "Use it for movies" (media.drive_use), nothing with a file
                 system anywhere -> "Format for Hemma" (?format=<disk> dialog, typed ERASE, media.drive_format with
                 the size shown), others say why. Requests like updates: "<what> <16 hex nonce>" into
                 <data>/disks/request (setting media.drive_asked "<unix> <person> <what>") -> the root helper
                 deploy/hemma-disks -> /var/lib/hemma-disks: result "<state> <unix> <uuid|-> <path|-> <words>"
                 (working used forgotten refused failed) + disks ("<uuid> <path> <fstype> <label>"). A "used"/
                 "forgotten" result newer than media.drive_done adds/drops the library folder (media_root_add/_drop,
                 also at the start: the helper restarts Hemma after a mount) + Latest; media.drive_seen = Got it.
                 One of Hemma's disks seen but not mounted (a Pi doesn't remount on replug) -> "use <uuid>" asked
                 by itself, once per 5 min. Checks: "Movie disk" WARN missing / INFO plugged in, "New disk found"
                 INFO, "Disk helper" WARN when unanswered 15 min. Tests: media_test_drives(sys, udev, minfo) +
                 media_test_drive_dirs(ask, answers) after th_boot; device numbers "8-1" (no ':' on Windows).
  media/room.c   disk room shared by uploads and conversions: MEDIA_RESERVE (1 GB) always stays free (the
                 database often shares the disk), and running jobs hold what they may still write (an upload
                 its unsent bytes, a conversion its original's size minus what it wrote), per disk (same
                 total size, free within 64 MB). media_room(), media_room_text(), media_test_free_space().
                 Uploads never take a file the server's converter is making (convert_making): skipped at the
                 plan; a quiet upload whose copy the converter took meanwhile gets PUT ok without writing
                 (UpFile.on_server; GET says at=size; its half-sent file deleted) and ?size= -> {"skip":true}. Every PUT
                 checks the rest of its file fits (else 507 + stop). upload_claims = files being written +
                 queued files of a live upload (running, or planned in the last 30 s) - by the VIDEO's name: strip
                 MEDIA_PART_SUFFIX first (issue #89: it never matched). A copy's half-sent file older than the
                 server's start is never resumed (the browser converts it again, maybe elsewhere).
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
                 media/media.h; tv's pages draw it too; Live TV only while tv is built; admins get "Add videos"
                 there once the library has a folder, not on Live TV - 1.1: it hid behind Library settings).
                 tv = .nav 0, .parent "media". Recently added groups a series' episodes that arrived within a week of
                 each other (media_recent). Blocks (media/tiles.c): media.continue (TV), media.recent (TV +
                 wall), media.series, media.movies, media.series_one (choices = every series by hex id;
                 next-up first), tv.onnow (TV + wall). Drawn posters: media_tile_art / media_art_style
                 (--h/--h2/--h3 from a title hash, comma hsl() and no calc() for old TVs). TV MODE
                 (?tv=1&back=<path> on /media/watch/<id>, /media/series/<id>, /tv/<n>): bare, always dark,
                 Back first (data-esc data-back: Escape, the remote's Back key and the arrows reach it -
                 issue #57), back checked by media_safe_back (else "/"); the player autoplays +
                 autofocuses, resumes for the viewer, data-next keeps tv/back; a channel: ArrowUp/Down
                 change channel, 1-9 pick one. Live TV's picture has NO scan lines or vignette over it (owner,
                 2026-10-14: "overkill, kills the quality of the show"); the old-TV touches left are the set's
                 frame, the brief static between channels, the fading "CH 03" and the On-now tiles' lines.
                 SCHEDULE (tv_plan.c, issue #56): a channel starts at once - lap 0's first airing is a
                 LEAD-IN from `epoch` (when it was made) for the programme's own length; on the half hour
                 the second starts at the first :00/:30 after it, and tv_epoch() = that minus starts[1] (so
                 every later airing is aligned; a slot = the length rounded UP to whole half hours: a
                 45-min show takes an hour, a 1 h 52 film two). Laps before the epoch (a clock set back)
                 end exactly at it. tv_window walks the same sequence (advance()). The row is "On TV" (issue #101):
                 playing channels first, then those between programmes: the next one's tile dimmed (.tv-tile-off) + "Up next · HH:MM" (.tv-tile-up).
                 PLAYERS (owner, 2026-10-12: full screen must survive the
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
                 ("Movies"). Also drops watch progress, logs to the activity feed. Episodes, seasons and series:
                 media_purge_* here too, driven by manage.c.
  av/            pure-C codec library, no MODULE(), libc only (no core/ deps, no globals) so it also
                 builds for wasm32 (audiotags.* isn't in av.wasm: the server's alone).
                 audiotags.* (1.6): an audio file's tags without decoding a sound - MP4/M4B by box headers only (never the
                 sound's sample tables: ~180 reads, 5-7 KB of a 1.5 GB book; chapters from the tref 'chap' text track with its
                 simple elst, else Nero chpl; ilst tags, description ldes > desc > ©des > ©cmt, covr located by offset) and MP3
                 (ID3v2.2-2.4 with unsynchronisation, CHAP, APIC located only when unsync left its bytes alone; ID3v1 without v2;
                 Xing/Info less LAME's delay+padding, VBRI, else CBR). Text out as clean UTF-8 (Latin-1 = cp1252, or UTF-8 when
                 it is). Equal to ffprobe on 7 real m4bs. audiotags_testmk.h = test builders (at_test_m4b takes aac.c's AUs;
                 clips_test.c makes the browser tests' books with it). mkv.* (2.5: AND AN MP4/MOV READER behind the same API - mkv_mp4.h, mkv_open knows an MP4 by its first box,
                 codecs named as Matroska names them; see "Where things stand" 2.5.0) (Matroska demuxer: cues/bisect seeking, lacing, unknown
                 sizes), mpeg2.* (MPEG-2 MP@ML + MPEG-1 video decoder, display-order frames, IEEE 1180
                 IDCT - idct.h, header-only, shared with mpeg4.c since 2.5), mpeg4.* (2.5; owner, 2026-10-31: "the videos
                 on hemma that are not converted" - Solsidan S06E03-04 (Simple Profile MP4s, resync markers, 11.7 fps
                 unevenly) and S07 (XviD 1.3 ASP with B-frames in MKV, 1144/1152x720) played nowhere: MPEG-4 Part 2,
                 ISO/IEC 14496-2 Simple + Advanced Simple - I/P/B-VOPs, S-VOPs only without warping points (GMC with
                 points refused), AC/DC prediction, H.263 and MPEG quantisers (+ matrices, mismatch control), 1 or 4
                 vectors, half/quarter samples (FFmpeg's chroma rules; DivX's/old XviD's chroma and direct-16x16 bugs
                 from the user data), direct mode, video packets: a marker naming a LATER macroblock is taken only when
                 the ones before it take no bits (B-VOPs whose co-located one was skipped - FFmpeg's encoder puts them
                 there), else concealment + resync search; not-coded VOPs bring no frame; DivX packed blocks (P+B in one
                 block, an empty one after). PTS: each block's time is pushed (sorted), each frame shown pops the
                 smallest - a packed block's second VOP takes the next empty block's time (`debt`), a lone N-VOP's time
                 is dropped. Refused by name (mpeg4_unsupported): interlaced, GMC, data partitioning, shapes, sprites,
                 complexity estimation, NEWPRED, reduced res, scalability, not 8-bit. MC clamps to the VISIBLE size (1144
                 wide: not MB-aligned). Checked vs FFmpeg (HEMMA_TEST_MPEG4=<file> [_REF=<ffmpeg yuv420p, -fps_mode
                 passthrough>] [_N] [_OUT] ./build/test mpeg4_real): the 3 real files 53-65 dB, 16 clips from FFmpeg's
                 mpeg4 and libxvid with every tool 49-66 dB luma PSNR, 0 concealed - the IDCT is all that differs.
                 mpeg4_testenc.h = test streams (m4_*: flat intra MBs with a DC-prediction replica, vectors, escape-3
                 coefficients; mpeg4_test_block() = whole streams for conv seeds + avfuzz). DivX 3 (MS-MPEG4) is another
                 codec: probe calls it "msmpeg4", never converted), ac3.* (full A/52 + E-AC-3 decoder (ETSI TS 102 366 Annex E: 1/2/3/6 blocks a frame,
                 Ac3Info.samples; spectral extension; AHT with ac3_aht.h = the spec's VQ tables - FFmpeg's
                 hebap-4 table lacks the spec's row 0; mixing metadata's Lo/Ro levels; independent substream 0
                 only, the rest AC3_SKIP; enhanced coupling / half rates AC3_ERR_UNSUPPORTED): native/WAV-order/
                 Lo-Ro stereo float output. ac3_test.c's encoder writes E-AC-3 with every tool and the decoder
                 must rebuild exactly what it sent; Dolby's E-AC-3 encoders leave unused bits non-zero, so
                 tail_zero is an AC-3 check). dts.* (1.3; owner, 2026-10-24: "let's fix this" - 108 Simpsons
                 episodes had DTS): the DTS Coherent Acoustics CORE (ETSI TS 102 114 clause 5 + Annex C/D) -
                 16-bit big-endian frames (sync 7F FE 80 01; DTS-HD's substream 64 58 20 25 after the core is
                 left alone; 14-bit/LE packing and AMODE 10-15 DTS_ERR_UNSUPPORTED; core extensions XCh/X96/XBR
                 skipped), every Huffman book, block codes, ADPCM prediction, high-frequency vectors, joint
                 intensity, transients, sum/difference, dynamic range (off by default), LFE 64x/128x, both
                 filter banks; native/WAV/Lo-Ro float output (the Lo/Ro normalised like ac3's). The spec says
                 never to test its CRC words: skipped. Tables: dts_tables.h, MADE BY A SCRIPT from the
                 specification's own pages and checked (each Huffman book a complete prefix code; two misprints
                 mended there - see its comment; the perfect-reconstruction bank is V1.2.1's, which decoders
                 use; V1.6.1 prints other numbers); the two vector tables NO edition prints are FFmpeg's
                 (third_party/ffmpeg/dts_vq.h, LGPL 2.1+, its README: source, hashes; owner: "FFmpeg's, own
                 file"). The synthesis's output gain (the spec's undefined rScale) = 128 sqrt 2 (QMF_GAIN),
                 measured. CHECKED against FFmpeg on real tracks: 5 Simpsons episodes, 615k frames, no frame
                 refused; every channel equal to rounding (largest difference ~6e-6). dts_testenc.h = a test
                 frame writer (every tool, random; or tones: a steady value in subband k comes out at its EDGE,
                 so tones use odd bands: (ch + 1) x 1500 Hz). HEMMA_TEST_DTS=<file.mkv> [HEMMA_TEST_DTS_S,
                 HEMMA_TEST_DTS_WAV] ./build/test dts_real decodes a real track (native float WAV to compare).
                 Feed mpeg2 (and mpeg4) the track's CodecPrivate first (an AVI's V_MS/VFW/FOURCC: past its
                 BITMAPINFOHEADER), then each block with its pts; mpeg2_reset()/mpeg4_reset()/ac3_reset() after mkv_seek().
                 conv.c re-encodes MPEG-4 progressive (DEINT_PASS), its track on a 90 kHz clock timed by each picture's
                 pts (place_of; the last one and those without a time: the median gap of the first pictures, usual_gap). h264enc.* (Constrained
                 Baseline encoder, bit-exact vs Chrome; annexb=false + h264enc_avcc() for MP4; ~180 fps SD
                 per PC core, ~7.6 fps per Pi 3 core). mp4.* (faststart via reserved moov, or fragmented), flac.*, deint.* (IVTC / yadif / bob /
                 pass + analysis), aac.* (AAC-LC encoder, 48 kHz stereo 160 kbit/s for the converter;
                 aac_testdec.h = a small decoder for tests). conv.* = the converter (MkvReader in, Mp4Out
                 out, progress callback, `unsupported` = a codec it doesn't handle, err in words via codec_words;
                 want_subs (1.2): up to AV_CONV_MAX_SUBS (32) subtitle tracks, the first 32 the converter takes,
                 file order, empty ones left out - AvConvSub.kind: AV_SUB_TEXT = srt/len (SRT, ASS/SSA, WebVTT -
                 vtt_text keeps <i><b><u>, drops other tags and <rt> text, unescapes the six entities, never reads
                 BlockAdditions; at most AV_CONV_SUB_BYTES 768 KB: an upload POST stays under the 1 MB body limit);
                 AV_SUB_PGS = data/size, a whole .sup (one record per segment: "PG", PTS u32 90 kHz, DTS 0; cut
                 segments dropped); AV_SUB_VOBSUB = data/size = the .sub as mkvextract writes it (2048-byte packs,
                 private stream 1 PES with the PTS on the first, substream 0x20, stuffing < 6 bytes or a padding
                 stream) + idx/idx_len (the CodecPrivate behind "# VobSub index file, v7" when it lacks it, then
                 "\nid: <2 letters|-->, index: 0" - MakeMKV's ends "# end" without a newline - and the
                 timestamp/filepos lines). Clock: 0 = the first video block with a time in file order (find_v0, as
                 survey), clamped at 0. AV_CONV_PIC_BYTES 64 MB a picture track, AV_CONV_PIC_TOTAL 256 MB for all,
                 counted as allocated (pic_room); past either the track goes whole (AvConvJob.pic_bytes lowers both).
                 Compressed tracks (zlib/bzlib/lzo) left out quietly - none in the household's files; header
                 stripping is mkv.c's. An `unsupported` refusal runs subs_pass from the start (err stays the
                 refusal; skip_refused_subs turns it off - the wasm build, whose refused files go up as originals).
                 av_subtitles(j) = only that pass (reads j->in, writes nothing, stop + progress; for videos never
                 converted) - the same bytes as a conversion's (conv_test and convfuzz check it). av_convert_free
                 frees them. Used by media/convert.c
                 and by wasm/avconv.c (conv_subs/conv_sub_text/_len/_lang/_name/_forced/_kind/_data/_size/_idx/
                 _idx_len exports; web/conv.js posts {size, subs, pics} - pics = [{kind 'pgs'|'vobsub', lang, name,
                 forced, data: ArrayBuffer (transferred), idx}]; conv_subs_only(size) = av_subtitles alone, posted
                 as {subsOnly, subs, pics} - 1.3, a video that stays behind), which
                 build.ps1/Makefile compile to $B/av.wasm (zig cc -target
                 wasm32-wasi -mexec-model=reactor -msimd128; ~370 KB, imports only 4 hemma.* functions,
                 embedded as /static/av.wasm). Measured on a DVD episode: wasm ~75% of native speed (374 s
                 vs 282 s on one core), video bit-identical to native, AAC differs only by float rounding.
                 conv.c bounds samples per track (2M; with a stated length 300 pictures / 120 sound blocks
                 per second; AvConvJob.max_samples lowers it) and refuses MPEG-2 whose picture size changes
                 part-way - both `unsupported` (the browser then sends the original). Timestamps beyond ~13
                 days count as missing (MAX_TS). DeintIn carries width/height; deint_push refuses any other.
                 subpic.* = picture subtitles from the sidecars (1.2): subpic_open_sup (Blu-ray PGS, a .sup) /
                 subpic_open_vobsub (DVD: the .idx text + an MkvReader over the .sub) index on open - segment and
                 pack headers, PCS/ODS headers, each SPU's control part - and subpic_render(i) reads that cue's
                 bytes and draws it as indexed colour (w*h px + 0xRRGGBBAA palette, every value < ncolors; the
                 reader's ctx lives until subpic_close). PGS: a segment is believed only when its size lands on the
                 next "PG" (else the next believable one, within 1 MB); epoch start (0x80) resets objects +
                 palettes, an acquisition point only replaces by id; each cue keeps its objects' data pieces and its
                 palette's segment chain; it ends at the next display set, the last 5 s on; its rectangle = the
                 union of its objects (crops honoured) on the first PCS's frame; uncovered pixels = the clearest
                 entry; BT.709 above 576 lines, else BT.601 (limited range). WDS windows are read past, not used to
                 clip; forced flags aren't reported. VobSub: the FIRST id: track's timestamp: lines (+ delay:),
                 MPEG-1/-2 packs, one substream 0x20-0x3F gathered to the SPU's size; end = its stop (cut at the
                 next cue's start), else the next cue's start, else 5 s; custom colors ON = its 4 colours, tridx's
                 first digit = value 0 clear; no size: = 720x480 (576 when a picture reaches past line 480).
                 SUBPIC_MAX_CUES 20,000, SUBPIC_MAX_SIDE 4096, every pool capped; cues sorted by start. png.c
                 png_indexed(): colour type 3, PLTE + tRNS, one fixed-Huffman deflate block whose only matches are
                 runs (distance 1). Checked against ffmpeg: a Simpsons PGS track (within 1 = colour rounding), a
                 70's Show VobSub track (identical). Not in av.wasm (the browser doesn't draw them; the server does).
web/             app.css (core styles + shared vocabulary), wire.js, conv.js (the upload converter's Worker:
                 feeds av.wasm a File - reading ahead: reading on from where it stopped, each read twice the last,
                 up to 32 MB (8 MB on a phone), so workers sharing a hard disk read in long runs (1.3) - and
                 gathers the copy, in memory or a Blob; or only a file's subtitles), icon.svg, manifest
third_party/sqlite/  SQLite 3.54.1 amalgamation + its recovery extension (ext/recover: dbdata.c, sqlite3recover.c/.h;
                 recover.c includes both as one object; sqlite3.c needs SQLITE_ENABLE_DBPAGE_VTAB) - see its
                 README for provenance + hashes
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
                 `HEMMA_FUZZ_SEEDS=<dir> HEMMA_FUZZ_RAW=<dir2> ./build/test conv_fuzz_seeds` (generated, never
                 committed: the MKVs in <dir>, raw Dolby frames, MPEG-2 and MPEG-4 streams in <dir2>/ac3, /mpeg2, /mpeg4)
tools/avfuzz.c   faster targets, one part alone each (issue #50): -DAVFUZZ_MKV the Matroska reader,
                 -DAVFUZZ_AC3 the Dolby decoder (raw frames, CRC off), -DAVFUZZ_DTS the DTS decoder (raw core
                 frames; 1.3), -DAVFUZZ_MPEG2 the MPEG-2 decoder (a raw
                 stream in chunks of any size), -DAVFUZZ_MPEG4 (2.5) the MPEG-4 Part 2 decoder (a VOP a block, whole or in
                 pieces, block times in or out of order; FUZZING_BUILD_MODE caps its pictures at 720x576 - ~480 inputs/s
                 in WSL); an input's first byte picks options (layout, chunking, a reset).
                 -DAVFUZZ_PGS (a .sup) and -DAVFUZZ_VOBSUB (the .idx text, a NUL, the .sub) (1.2): opened, every cue
                 checked against subpic.h's promises (abort), drawn (2M pixels an input) and made a PNG.
                 Measured in WSL: ~1.3M inputs a minute (mkv), 210k (ac3), 310k (mpeg2), ~200k (pgs), ~90k
                 (vobsub), ~8k (dts: a 100-byte frame may say 4096 samples on 6 channels, so it takes 32 frames
                 and 32 KB an input; its coverage levels off within a minute) vs 5.7k for convfuzz. The DTS
                 decoder itself: ~220x real time on one PC core (5.1, 1.5 Mbit/s, -O2). convfuzz also checks every subtitle track is a whole file of its
                 kind within its bounds and that av_subtitles alone gives the very same subtitles.
                 -DAVFUZZ_TAGS (1.6): audiotags.c over any bytes - the first byte picks the name hint (none, x.mp3, x.m4b,
                 book.MP3); aborts on text that isn't clean UTF-8, chapters out of order, a cover past the end or without its
                 JPEG/PNG bytes there. Seeds: HEMMA_FUZZ_RAW=build/fuzz-raw build/test audiotags_fuzz_seeds (~40-50k inputs/s;
                 50 min with 8 processes before 1.6 found one bug, in its own text check)
tools/ci/fuzz.sh TARGET [S] [JOBS]: builds + runs conv | mkv | mp4 | ac3 | dts | mpeg2 | mpeg4 | pgs | vobsub | tags under ASan + UBSan from the
                 tree's top after `make build/test` (seeds made when missing; pgs/vobsub's from `HEMMA_FUZZ_RAW=
                 build/fuzz-raw build/test subpic_fuzz_seeds`), corpus in build/fuzz-corpus-T, findings in
                 build/fuzz-found-T (exit 1); JOBS > 1 = libFuzzer -fork, carrying on after a finding. Used by
                 build.yml's fuzz job (conv, 90 s), fuzz.yml (NIGHTLY 01:17 UTC: the ten side by side on their own
                 runners, 170 min each with every core, the corpus cached night to night - owner, 2026-10-21:
                 "about 3 hours"; a finding fails the job and keeps the files as an artifact) and local.sh
tests/           test.h (framework), runner.c, harness.* (boot the core without a server; th_boot_in(dir) for a
                 data folder the test prepared - test_tmpdir() is a NEW folder each call). runner: `--shard K/N`
                 runs every Nth test from the Kth and ends with "shard K/N: R run, F failed, C checks, M ms";
                 parallel.sh (make test) and build.ps1 test start one per core and add them up - so a test
                 must not lean on what an earlier one left behind (a module's statics survive th_halt: reset
                 them in start(), as media's disk meter does)
tools/ci/bundle.sh  PROGRAMS_DIR OUT_DIR [NOTES]: the release downloads (a tar.gz per platform, Windows a zip):
                 program + LICENSE + README.txt + VERSION + SHA256SUMS; Pi/Linux bundles also install-pi.sh +
                 hemma.service + hemma-setup.* + hemma-upgrade + hemma-update.* + hemma-disks + hemma-disks.* (the installer copies them from its
                 own folder), Mac ones local.hemma.plist; a release-wide SHA256SUMS (over the .tar.gz); the notes.
                 With HEMMA_SIGNER (a hemma that runs there) + HEMMA_SIGNING_KEY every SHA256SUMS gets its .sig
                 and the folder is --verify-release'd. Never deletes (mktemp -d). Locally: `sh tools/ci/bundle.sh build <out>`.
                 HEMMA_IMAGE=1 (release.yml): tools/image/build.sh too, before the release-wide SHA256SUMS
                 (which then lists the .img.xz) + a line in the notes' downloads table. ONLY THE NEWEST RELEASE
                 KEEPS ITS IMAGE (owner, 2026-10-20): release.yml's step after publishing deletes every older
                 release's *.img.xz (gh release delete-asset; skipped when the new release has none).
tools/image/     THE RASPBERRY PI IMAGE (issue #71): build.sh BUNDLE OUT (no root, any Linux: base = Raspberry Pi
                 OS Lite arm64 `_latest`, its .sha256 checked, HEMMA_IMAGE_BASE/HEMMA_IMAGE_CACHE; the root
                 partition dd'd out, written with `debugfs -w` (sif mode/uid/gid 0), e2fsck -fn clean, dd'd back,
                 xz -T0 -6 -> OUT/hemma-<ver>-pi.img.xz). Adds /opt/hemma-image/{hemma-pi64/ (the bundle),
                 debs/, firstboot.sh, README.txt} + /etc/systemd/system/hemma-firstboot.service (enabled by a
                 symlink in multi-user.target.wants). debs.awk: what unattended-upgrades + acl need that the
                 image's dpkg status lacks (Provides count; "=" versions checked), from Debian's
                 binary-arm64/Packages.xz whose SHA-256 is in InRelease verified by gpgv with the image's own
                 debian-archive-keyring.pgp. FIRST START (firstboot.sh, root oneshot, After=cloud-config.service -
                 cloud-final/cloud-init.target come AFTER multi-user, so never order after those): dpkg -i the
                 debs (debconf preseed, 20auto-upgrades), hostname hemma only while it's "raspberrypi" (+
                 /etc/hosts, cloud.cfg.d/99_hemma.cfg preserve_hostname), then the bundle's install-pi.sh
                 (HEMMA_INSTALL_WAIT=120) - secrets made there - and `systemctl disable` itself (a failure
                 retries at the next start). Base facts (2026-10 trixie Lite): no SSH host keys
                 (regenerate_ssh_host_keys), machine-id "uninitialized", avahi + cloud-init (Imager's settings)
                 + ntfs-3g + exfatprogs present, no unattended-upgrades/acl/python3-dbus/python3-distro-info.
apps/android/    THE HEMMA APP FOR ANDROID (2.0; docs/decisions/0011, docs/android.md): plain Java on the platform's own APIs,
                 NO libraries, NO Gradle - build.sh (Linux/macOS, `make app`) and build.ps1 Build-App (`./build.ps1 app`) run
                 javac (-source 8, bootclasspath android.jar + build-tools' core-lambda-stubs.jar), aapt2, d8, aapt add
                 (classes.dex), zipalign, apksigner: ~40 KB. minSdk 26, targetSdk 36, package io.github.kloparn.hemma,
                 versionCode MAJOR*10000+MINOR*100+PATCH of VERSION. Built automatically when ANDROID_HOME (else Android
                 Studio's %LOCALAPPDATA%\Android\Sdk) and a JDK (JAVA_HOME, else Android Studio's jbr) are found, and
                 EMBEDDED in the program as /static/hemma.apk (like av.wasm; SKIP=app / HEMMA_SKIP_MODULES=app leave it
                 out); in CI only with the release key. KEY: HEMMA_APP_KEYSTORE (PKCS12) + HEMMA_APP_KEY_PASSWORD = the
                 release key (an update installs only over an app signed with the same key); else ~/.hemma/hemma-app-
                 dev.p12 made once (password hemma-dev) and the app is debuggable (--debug-mode: chrome://inspect, or adb
                 forward tcp:9229 localabstract:webview_devtools_remote_<pid> + /json). Files: Net.java (pure Java: which
                 addresses are the home network - 10/8 172.16/12 192.168/16 169.254/16 127/8, fe80::/10 fc00::/7 ::1, never
                 100.64/10 -, home names: one label or .local .lan .home .home.arpa .internal, the Wi-Fi's addresses
                 nearest first (/24, at most /22), a typed address, /hemma.json parsed) + test/NetTest.java (run by both
                 builds with a JDK alone); Finder.java (NSD "_hemma._tcp" and asking every address on ports 80/8080 at
                 once, 48 threads, 700 ms; every answer checked: /hemma.json from a private address; one report per id);
                 MainActivity.java (the WebView - JS, DOM storage, autoplay, UA + " HemmaApp/<version>", cookies kept;
                 other origins open in the browser; the finder panel built in code; prefs base/id/name: the same Hemma
                 found again at a new address by its id; file chooser ACTION_GET_CONTENT */*; downloads via DownloadManager
                 with the cookie; full screen = onShowCustomView; Back = history, then moveTaskToBack; edge to edge with
                 insets as padding; window.HemmaApp = Bridge {media(json), version()}, only on Hemma's own pages; the
                 page's onPause only while no book plays); PlayerService.java (foreground service, type mediaPlayback,
                 while a book is loaded: MediaSession + MediaStyle notification - cover (fetched with the cookie, <= 512
                 px), chapter, author, book; play/pause, skip = chapters, rewind/ff + custom actions 15/30 s, seek; a
                 partial wake lock while playing; "none" or the task swiped away ends it). ANDROID 17 asks for
                 ACCESS_LOCAL_NETWORK ("find, connect to ... nearby devices") - without it no connection to the home
                 network gets through (seen on a Pixel 10, 2026-10-28: AppOps "Operation not started ... ACCESS_LOCAL_NETWORK");
                 the app asks at the first look, again from Look again, then sends to the app's settings page; an app in
                 the back of a locked phone has no network either - it looks again when it comes to the front.
                 Strings in res/values{,-sv,-es}. The app's name in a release: Hemma.
                 OFFLINE (2.3; media/offline.c's manifests): MainActivity takes hemma-app:save?m=<path> (Saved.savePath: only
                 a /media/offline/... path - letters, digits, /-_., no query, no ..) and hemma-app:offline, only from Hemma's
                 own page; the first save asks POST_NOTIFICATIONS (Android 13+: the progress notification). Saved.java =
                 the pure part (a small JSON reader/writer, the manifest/item model, a book's whole-book place <-> part +
                 offset, Range resume, which places to send) - test/SavedTest.java, run by both builds like NetTest.
                 Offline.java = the store (getExternalFilesDir/offline/<id>/: item.json, files, poster ~512 px, .vtt) + HTTP
                 (the WebView's cookie, private addresses only) + the sync: open() POSTs every unsent place to
                 /a/media.progress (Origin/Referer = base), one sync at a time. OfflineService = a dataSync foreground
                 service downloading ITSELF (HttpURLConnection, .part + Range resume, one file at a time, StatFs + 200 MB
                 margin) - not DownloadManager (another process: Android 17's local-network permission, and the cookie).
                 OfflineActivity = the Offline screen (also the finder panel's "Offline (N)" when Hemma can't be reached);
                 WatchActivity = VideoView + MediaController (subclassed: it ate Back), VideoView's own WebVTT; BookActivity
                 + PlayerService's native mode (MediaPlayer over the parts, the place in the whole book, chapters as
                 next/previous, -15/+30, audio focus, pause when headphones go; the page's reports ignored meanwhile). Tried
                 on an Android 8 emulator against a stand-in; on a real phone still to see: Android 17's local network for
                 the app's own HTTP, Android 15's dataSync limit, multi-GB files, HEVC/AV1 copies, WebVTT, insets on 11+.
                 Known: a video brought back from the background reopens without its subtitles. TWO RULES OF HEMMA'S ANSWERS:
                 signed out, the core answers a module's address with the sign-in page - a 200 of text/html, never a 401
                 (Offline.signInPage: refused as 401 for manifests, files, posters, subtitles); and a file's size is what
                 the answer says (Saved.totalOf: a 200's length, a 206's /total) - a manifest's "bytes" is the playable
                 file's, and a video in several languages is sent as its smaller one-sound view.
tests/browser/   CI-ONLY Node (owner, 2026-10-21: "The CI can use node"; nothing ships): Playwright in Google
                 Chrome (the bundled Chromium can't play H.264/AAC) against a real Hemma - server.mjs makes a fresh
                 folder under the OS temp (hemma-browser-tests), test clips by `build/test av_test_clips` with
                 HEMMA_TEST_CLIPS (modules/av/clips_test.c: H.264 + AAC MP4s from our own encoders, 64x48 - two
                 100 s episodes, a 40 s film, an upload clip) and MKVs by `build/test conv_fuzz_seeds` with
                 HEMMA_FUZZ_SEEDS=<work>/mkv (1.3: the converter's seeds), an admin from the settings file, then
                 build/hemma on 127.0.0.1:18093; setup.mjs signs in, adds the library, waits for the scan. Specs:
                 player (progress
                 reported + resume, resume without wire.js via #t=, nothing in the last 5 s, the next episode in
                 the SAME element), live (now, no pause/wind/speed, #t= without wire.js), upload (byte for byte;
                 1.3: the converter in Chrome on h264-webvtt-pgs-vobsub.mkv - the copy + its subtitles, a copy in
                 memory in 1000-byte pieces arriving identical, subtitles only, copies and plain files in one
                 upload; skipped where the page has no data-wasm - CI's browser job sets up zig for av.wasm),
                 signin (Accept-Language, the language links), subtitles (two in one language, the pick by key into
                 the next episode; 1.3: C on and off, the pick following the series into a new page),
                 upload (1.3: a DTS MKV converted in Chrome too), subpictures (a picture track at its cue's time and place over the shown picture,
                 lifted over the bar, never beside a text track, on from the viewer's language, phone, full screen -
                 clips_test.c writes <clips>/sidecars/pictures.sup 640x480 and pictures-576.sup 720x576, two cues
                 each), download. eslint.config.mjs (recommended rules) over
                 web/*.js, run from the repo top (`npm run lint`). Locally: build (./build.ps1 build + test, or
                 make), then `cd tests/browser && npm ci && npx playwright test`. Test-only modules' words etc.:
                 see Tests. CI jobs: build.yml `browser` and `lint`
tools/ci/local.sh  CI's Linux jobs locally: test | asan | fuzz [S] [conv|mkv|mp4|ac3|dts|mpeg2|mpeg4|pgs|vobsub|tags], on a fresh copy of the tree (stdin tar,
                 --tar FILE, or this checkout) in /dev/shm, each job's build/ kept for the next run (only
                 what changed compiles); never deletes. Windows: ./build.ps1 ci [job] (WSL; $env:HEMMA_WSL)
deploy/          avahi-hemma.service (2.0): install-pi.sh writes it to /etc/avahi/services/hemma.service (Hemma's port for
                 @PORT@, then reloads avahi-daemon) when /etc/avahi/services exists - "_hemma._tcp", TXT path=/hemma.json:
                 how the Android app's NSD finds Hemma; bundles carry it (bundle.sh units).
                 hemma-disks (issue #74; sh, root, -> /usr/local/sbin): `sudo hemma-disks` [use UUID | format sdX | forget
                 UUID]; --from-hemma = hemma-disks.service (root oneshot, NO namespace options: a mount made in a
                 service's own mount namespace would vanish with it) on hemma-disks.path (PathModified
                 /var/lib/hemma/disks/request). Checks everything again with lsblk/blkid/findmnt: never the disk under
                 / /boot* /usr /var /home or swap, never a non-USB disk, nothing mounted, nothing in /etc/fstab by hand;
                 format only sd[a-z]{1,2} of exactly the size Hemma showed with NO file system on any part of it
                 (blkid -p), then wipefs + sfdisk (gpt, one linux partition) + mkfs.ext4 -L movies -m 0 -T largefile.
                 use = /etc/fstab copied (fstab.before-hemma-disks-<when>, 3 kept), two lines appended ("# hemma-disks:
                 <name> (added <day>)" + UUID=... /mnt/<slug of label> <type> defaults,nofail,noatime,
                 x-systemd.device-timeout=10s[,uid,gid,umask=0022 for exfat/vfat/ntfs] 0 <2 for ext*>), the line checked
                 (findmnt --verify; "does not match with on-disk" ignored: ntfs3 vs ntfs), mounted by `systemctl start
                 <unit>.mount` (fstab put back if it fails), ext*/xfs/btrfs: an empty one chown hemma, else setfacl
                 -R; then hemma.service.d/disks.conf regenerated from the marks (Wants/After the mount units +
                 ReadWritePaths; gone with the last disk; the installer's media.conf never touched) and Hemma restarted.
                 forget = the two lines out (not unmounted). A disk of Hemma's that's back = mounted again.
                 hemma-upgrade (issue #73; sh, root, -> /usr/local/sbin): `sudo hemma-upgrade` [--check | --dry-run |
                 --file F | --release X.Y.Z | --force]; `--from-hemma` = hemma-update.service (root oneshot, 20 min) on
                 hemma-update.path (PathModified /var/lib/hemma/update/request). Reads Hemma's folder only AS hemma
                 (setpriv/runuser, timeout) and a request only once (its nonce kept in /var/lib/hemma-update/handled);
                 answers by rename into /var/lib/hemma-update (root:hemma 750). Download: api.github.com releases/
                 latest (HEMMA_UPDATE_REPO), then SHA256SUMS + .sig + <bundle>.tar.gz -> the INSTALLED
                 hemma --verify-release --partial; the bundle is listed first (one top folder = the bundle this
                 machine needs - the installed program's ELF machine, else uname -m; plain names; only files/dirs;
                 <= 40 entries, 200 MB) before tar -x --no-same-owner, then --verify-release on it, VERSION
                 (newer unless --force; Hemma's requests never force), its program's --version must match. Then
                 the bundle's own install-pi.sh (HEMMA_INSTALL_WAIT=60) + /healthz again 5 s later; else GO BACK:
                 its own copy of the old program + install-pi.sh's newest hemma.db.before-install-* (what the failed
                 start left = hemma.db.after-failed-update-<when>, 3 kept), restart, wait. flock /run/hemma-upgrade.lock;
                 the script body is all read before it runs (main; exit) as the installer replaces it.
                 hemma-setup.service + hemma-setup.path (issue #72: root oneshot at boot and on PathModified of
                 /var/lib/hemma/hemma.setup -> `hemma --setup-note`: HEMMA-SETUP.txt on a Pi's boot partition (/boot/firmware or
                 /boot with config.txt; CRLF) + /etc/issue.d/hemma.issue (agetty, \4 = the IPv4); takes from Hemma's file only
                 "open XXXX-XXXX" / "done" (auth_setup_state_parse), else nothing; before Hemma's first start the env's
                 HEMMA_SETUP_CODE). install-pi.sh installs/enables them when they sit beside it (bundles carry them) and
                 writes HEMMA_MEDIA=<the folders, ;> into hemma.env (issue #27; ; and # refused in folder names).
                 It also installs hemma-upgrade (renamed into place) + hemma-update.* when beside it, makes
                 /var/lib/hemma/update (hemma 750) and /var/lib/hemma-update (root:hemma 750), enables the .path;
                 the same for hemma-disks + hemma-disks.* (/var/lib/hemma/disks, /var/lib/hemma-disks);
                 HEMMA_INSTALL_WAIT = seconds to wait for /healthz (15).
                 hemma.service (hardened systemd unit; waits for time-sync.target; RestartPreventExitStatus=78 =
                 HEMMA_EXIT_DATA), install-pi.sh (any systemd Linux, despite the name: keeps the program it
                 replaces as /opt/hemma/hemma.prev (going back = a SWAP hemma <-> hemma.prev, issue #92:
                 docs/raspberry-pi.md "Going back"), stops the service and copies hemma.db (+ -wal) to
                 hemma.db.before-install-<YYYY-MM-DD-HHMMSS> before the new program opens it (newest 3 kept),
                 enables systemd-time-wait-sync with a 90 s limit, starts the service,
                 waits for /healthz, prints addresses + setup code + next steps, or the journal tail and exit 1),
                 local.hemma.plist (macOS launchd agent). The Windows guide runs Hemma as Local Service (S-1-5-19,
                 icacls on C:\Hemma and the media folders), never SYSTEM.
docs/            features (everything Hemma does - a new feature gets its line here, not in the README), video-formats
                 (what plays, what's converted into what, what can't be yet) + user
                 guides: raspberry-pi linux windows macos home-network ssh accounts troubleshooting development
                 (README.md links them; "Why C?" and Contributing live in development.md). Public docs: generic
                 examples only (192.168.1.50, hemma.local, /srv/media) - never this household's names,
                 addresses, paths or passwords
docs/img/        the README's screenshots (WebP q88: phones 390x844@2, desktop/wall 1280x800, TV 1280x720) +
                 CREDITS.md. Never real household data or media: a made-up household ("Björkbacken": Anna
                 admin, Jonas, Maja, Leo, a "Living room TV" and a "Hallway tablet" display account) on a
                 throwaway demo built WITHOUT the net module (HEMMA_SKIP_MODULES=net: the Network page would
                 show the LAN's real devices), data on a scratch folder, filled through POST /a/<action>
                 (Origin + cookie; live actions such as media.progress run that way too), and only open
                 films: the Blender Foundation's open movies (Commons 480p VP9 transcodes + CC BY posters,
                 one film left without a poster to show the drawn card). Shoot members' pages (an admin's
                 home shows the "Address" check when the demo listens on 127.0.0.1). `hemma --demo` (core/demo.c)
                 makes the same household's people and plans, without media or display accounts.
VERSION          the version, MAJOR.MINOR.PATCH (semver.org) - raised in every PR (see Owner preferences)
LICENSE          Apache 2.0 (owner's choice, 2026-10-14); SQLite is public domain
ARCHITECTURE.md CONTRIBUTING.md CODE_OF_CONDUCT.md SECURITY.md   for people (issue #51): the map, how to send a change, how
                 to behave, how to report a vulnerability. docs/decisions/NNNN-*.md = one record per big choice (status,
                 context, decision, consequences; README.md indexes them) - a changed decision gets a NEW record.
                 .github/ISSUE_TEMPLATE (bug report with the version + the log, feature request, security -> the
                 private advisory) and pull_request_template.md (the checklist)
build*/ data/ .env   generated / runtime / secrets (gitignored)
```

## Build, run, test

```powershell
./build.ps1              # debug build -> build/hemma.exe (UBSan traps on)
./build.ps1 run          # build + run on http://127.0.0.1:8080, assets + module CSS live from disk
./build.ps1 test [flt]   # build + run every *_test.c, one process per core (flt = substring of test/file
                         #   name: one process)
./build.ps1 ci [job]     # CI's Linux jobs in WSL (tools/ci/local.sh): test | asan | fuzz [seconds]
./build.ps1 release      # optimised Windows build
./build.ps1 pi | pi32    # static Linux binaries for the Raspberry Pi (aarch64 | armv6), ~2.2 MB
./build.ps1 linux        # static x86-64 Linux (pi/pi32 also run on any ARM Linux)
./build.ps1 mac          # macOS 11+: build/hemma-mac-arm64 + hemma-mac-x64 (cross-compiled, not yet run on a Mac)
./build.ps1 app          # the Android app -> build/hemma.apk (apps/android; any build embeds it when the tools are here)
```
- `$env:HEMMA_SKIP_MODULES="media,tv"` leaves module folders out (someone else's work in progress).
  media needs av and tv needs media: skipping one skips the ones that depend on it.
- CI (.github/workflows/build.yml, every push): `make` + `make test` on Ubuntu and macOS; the suite again
  under ASan + UBSan (clang, leak checks off); a 90 s libFuzzer run over the converter (tools/convfuzz.c);
  the pages in Chrome (tests/browser) and eslint over web/*.js. Every night (fuzz.yml) ~3 h of fuzzing.
  The three Linux jobs run locally too, in about a minute: tools/ci/local.sh (Linux), ./build.ps1 ci (Windows,
  in WSL - zig has no ASan/libFuzzer); macOS only in CI. Run one before pushing. The Makefile builds one
  object per file (build/obj/h, build/obj/t; -MMD headers; .flags stamps rebuild on a new compiler or flags),
  so give make -j; CI does.
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
- On Linux/the Pi/macOS: `make -j8`, `make test` (`T=filter`: one process), `make run`, `make SKIP="tv"`; `sudo make install` (systemd
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
  `HEMMA_TEST_MKV=<file.mkv>` / `HEMMA_TEST_AC3=<file>` run the av decoders over a real file; `HEMMA_TEST_CONVERT=<a copy of an .mkv>` (+ `HEMMA_TEST_CONVERT_S=90`: seconds, 30 by default) `./build/test.exe convert_real_file_when_given` converts its start like the server does - its .srt files land next to that copy; `HEMMA_TEST_SUBS=<file.mkv> [HEMMA_TEST_SUBS_OUT=<dir>] ./build/test.exe conv_subtitles_of_a_real_file` takes its subtitle tracks out (sub-<k>.<lang>.srt/.sup/.idx+.sub); `HEMMA_TEST_SUP=<file.sup> | HEMMA_TEST_IDX=<file.idx> [HEMMA_TEST_PNG=<folder>] ./build/test.exe subpic_real` lists (and draws) a picture track's cues.

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
`{"t":"log","url":...,"m":...,"ua":...}` from a signed-in screen = the browser's own trouble, logged as a
warning ("browser of <username> (<ip>) on <url>: ..."): wire.js report() sends window errors, unhandled
rejections, a video's `error`, and a video meant to play (autoplay, live, or play pressed) that hasn't
fired `playing` 25 s later (watch(): readyState, networkState, paused, buffered) - a TV's browser has no
readable console; read it on the System page or the journal. Limits (app.c browser_log_ok, issue #79 -
the old per-connection cap restarted with every reload): per PERSON, 20 lines an hour (BROWSER_LOGS,
g_blog: 32 people, the oldest hour gives way); a line the same as that person's last one is only counted;
what was left out is one line when the hour ends ("N more lines since HH:MM left out") - from the tick
(browser_log_tick, issue #102: it waited for that person's next line); the "(no more from X's browsers until
HH:MM)" note comes right after "browser of X (ip)" and the page address is cut to 160 bytes (lines are cut at
1,024); the user agent (`ua`) goes with a session's first line only (ua_first, the last 64 sessions).
FORM ERRORS (issue #60): a failed act's error rides on its ack - `["ack",id,false,"<text>","<field>"]`, no
toast op - and wire.js formErr puts it beside the field (`.form-err` in its `.field` label, the field
aria-invalid + aria-describedby), or, without a field, above `.modal-f` in a form of class "stack"; else a
toast (quick bars: their live preview already says it). It's the browser's own node ([data-client]: kids()
steps over such nodes and key() ignores their ids; morph() re-marks a field whose ._err is set) and goes
when that field (or, form-level, any field) changes or the form is sent again. A success toast with
`{"undo":"<token>"}` gets an Undo button (sends core.undo, k=token; 8 s). Toasts over the focused field
(a phone's keyboard pushes fields down) go to the top (#toasts.toasts-top, dodge() on toast and focusin).
A `nav`/`hello` re-renders only that screen (plus `/` and `/people`, which show who is where;
the shell's "Here now" names people only); navs are rate-limited per screen. Every page carries
its hash (`<html data-h>`): the first `hello` after a load sends it, so an unchanged page isn't
sent twice. Static files are served gzipped (`Content-Encoding`, `Vary`) to browsers that accept
it: the build embeds a `.gz` twin of each text file and av.wasm (~120 KB -> ~30 KB first load).

## Accounts

- A `Person` is a household member; with `username` + `pass` they can sign in.
  Roles: `ROLE_MEMBER`, `ROLE_ADMIN` (manages people), `ROLE_DISPLAY` (read-only, e.g. a wall tablet),
  `ROLE_CHILD` (3) and `ROLE_GUEST` (4) (issue #44; owner, 2026-10-19: "as issue #44 says", guests "book, shop,
  board, watch"). Children and guests run only actions flagged `Action.may` (ACT_CHILD | ACT_GUEST), refused
  in run_action like a display account's ("Ask a grown-up to do that" / "That isn't open to guests"; the
  forms are cut by ui_role); modules flagged MOD_NO_CHILD / MOD_NO_GUEST aren't there for them (net: both,
  screens: guests). What's theirs is checked in the module: a child sees/ticks/edits only their own to-dos
  (plan_mine, childs_own; deletes only what they made), books only Resource.kids things for themselves
  (book_open_to; guests Resource.guests), changes only their own notes and bookings; a guest gets plan's
  shopping list only (no to-dos/calendar/agenda), no strip/Latest on Home, under Household only Your
  account, no recovery codes, no calendar link. A GUEST'S LAST DAY: `Person.until` = the local midnight after
  it (people.save `until`, required for guests; people's rezone hook moves it); person_signin_over() ends
  sessions (session_over) and auth_login answers LOGIN_ENDED - only after the right password - -> /login?e=5
  "Your sign-in has ended. To stay longer, ask <admins>". `/api`: 403 for children and guests too.
- MOVED OUT (issue #45): `Person.left` = when; the row stays (names on history: person_label "Eva (moved
  out)", ui_avatar's title + .av-left), no username/pass, never in person_count/person_at/person_real/
  person_find, privacy_on_tv false. people.delete how=keep|erase -> person_forget(&Forget{person, keep,
  apply, now}): every Module.forget hook, then the core (sessions, recovery codes, prefs, TV picks of them,
  erasing: their Latest lines; then the row kept as moved out - an admin becomes a member - or deleted).
  apply false = count + forget_note() only: the remove dialog (/people?remove=<id>) lists both ways first.
  Keep = what's AHEAD goes or is handed on (bookings to come cancelled, series stopped, open to-dos to
  anyone or the next in a turns line - plan_drop_turn - future events of theirs deleted, devices
  unassigned); erase = the rest of theirs too (past bookings, done to-dos, notes, watch history) and "by"
  on what they made for others cleared. people.return brings someone back (no sign-in). Display accounts
  and the moved-out are only erased.
- PRIVACY (issue #42, Your account -> Privacy, people.privacy; "all" = the whole form, unticked = no):
  core prefs PREF_WALL ("" / "name" / "no": screens' wall_people), PREF_DOING ("no": presence_screens and
  presence_page say nothing of them), PREF_ON_TV ("1"/"0"; unset = members and children yes, admins and
  guests no: privacy_on_tv, which screens_on_picker is), PREF_NOT_HOME ("1": presence_set_home ignores
  them, net's update_presence forgets them). net's "Your devices" card (net.mine_name, net.mine_drop: the
  owner's own; the rest stays the admin's).
- `person_count()/person_at()` = real people only (display accounts and the moved-out excluded) — use for
  pickers, "who's home", etc. `account_count()/account_at()` = every account (household admin).
- Sessions: random 128-bit cookie `hemma` (HttpOnly, SameSite=Lax); only its SHA-256 is stored.
  `session_create/session_end/sessions_drop`. Signing in again in another tab keeps that session.
  A session ends (`session_over`, checked at every lookup, rows pruned hourly) a week unused, a year
  unused with "keep me signed in", and - for admins - `ADMIN_SESSION_DAYS` (30) after signing in however
  much it's used (owner, 2026-10-12: plain http, a captured admin cookie must die; the cookie's Max-Age
  says 30 days for admins too) - and for display accounts `DISPLAY_SESSION_DAYS` (90; owner, 2026-10-17,
  issue #90: a TV's cookie; Max-Age 90 days too). The owner wants more conditions on top later - ask
  before adding any.
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
  visit while `auth_setup_open()`: salt set, setting `auth.bootstrapped` unset AND no admin who can sign in
  AND no unread rescue (SETTING_RESCUED; issue #87 - see "store" for the damaged page instead).
  It needs the one-time **setup code** first (`auth_setup_code()`: HEMMA_SETUP_CODE or setting auth.setup_code,
  logged at start, never shown on a page; wrong ones -> `/setup?e=setup`, throttled; cleared on success).
  ON THE CARD (issue #72): auth_setup_state_write() keeps <data>/hemma.setup = "open <code>" / "done" (at auth_start
  and after setup_post; only when changed) for deploy/hemma-setup.*; the welcome page names HEMMA-SETUP.txt when
  auth_setup_note_found().
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
  ADDRESS KEY (`ctx_url_key(c, path, out)`, app.c url_key/session_by_url_key): `<path>?k=<session id>-
  <expires>-<128 bits of HMAC(server salt, session token hash + path + expires + c->ip)>`. A GET/HEAD
  without a live session cookie whose key fits its exact path, comes from the address the key was made
  for (the screen's: a TV's browser and its player share one) and hasn't expired (KEY_HOURS = 48, rounded
  up to the hour so a page's hash changes hourly, not per render) runs the module endpoints as that
  session (live, not session_over); an address no endpoint takes goes on as signed out - a key never
  opens a page, an action, /api or /ws. (0.6.0's keys had no expiry or address: issue #78.)
  Why (2026-10-13): a Samsung TV's browser (an update that week) lets its video player fetch the
  <video>'s file WITHOUT the page's cookie (the navigation had it, the player's `Range: bytes=0-` didn't),
  so Hemma answered with the sign-in page and the TV said "decode error" - every video, Live TV, every
  Hemma version. media_file_src/media_sub_src put keys on video and subtitle addresses (media, tiles,
  tv); pictures need none (the browser fetches those itself). No stored session (the harness's
  app_ctx) = no key = the plain address. A signed-out WebSocket (hello/nav/act/close)
  only ever re-renders its own screen. `/api` is for members and admins (display accounts, children and
  guests: 403); tables
  with `api_owner` (pref, media_watch) show a member only their own rows, admins all (owner, 2026-10-12).
  Signed-out WebSockets: 48 in all, 4 per address (`stranger_room`), past that the upgrade gets 503.
  `MOD_ADMIN` modules exist only for admins: nav, /more, pages ("not found" for others) and http hooks.
- Log lines go through `log_clean()`: control characters become visible escapes (no forged lines).
  A typed sign-in name is logged only as the account's username when it names one, else as "an unknown
  name (N characters)" (auth.c typed_for_log, issue #34: passwords typed into the name box reached the log).
  Nor does it go into an ADDRESS (issue #100: /login?u=..., /forgot?u=...): a failed /login or /forgot keeps
  it per device address for TYPED_KEEP_S (60 s, typed_keep/typed_back, cleared with the throttle at each
  boot) and the page puts it back in the box; redirects carry only e=, w=, next=. (/setup still carries the
  new admin's name/username/home: that's what's being created, not a sign-in.)

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
- **Actions** mutate data, then `ctx_ok/ctx_fail/ctx_redirect`. An error about one form field:
  `ctx_fail_at(c, "password", ...)` (wire.js shows it beside that field - issue #60). Deleting: `ctx_undo(c, &T,
  id)` BEFORE `tbl_del` keeps the row, and the success toast offers Undo for UNDO_S (30 s) - no "are you sure?"
  for small things (the core's `core.undo` action puts the rows back with their ids; only for whoever removed
  them; display accounts get none). `app_notify_but(c, a, b, ...)` = app_notify leaving out people who got
  their own `app_notify_person` toast. They run inside one SQLite
  transaction; the core re-renders every screen afterwards. Never hand-craft DOM updates.
  `c->me` is always set in actions (signed in, not a display account); check `is_admin(c)` when needed.
  CHILDREN AND GUESTS run an action only when its table entry says so: `{"plan.shop_add", act, .may =
  ACT_ALL}` (ACT_CHILD | ACT_GUEST; issue #44) - a new action is closed to them until you open it, and
  then it still checks what's theirs (`is_child(c)`, `is_guest(c)`).
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
  Set `T.what` in init too: what its rows are in the household's words ("notes on the board") - a rescue
  reports in them, and core/app_data_test.c fails on a table without one.
  A rename: `FIELD_WAS(T, member, type, "old", format)` + a `breaks_older` migration at that format or later in
  core/store.c's MIGRATIONS (app_data_test checks);
  changed meanings of data: a migration with a `run()` (see "store" above). A raw `db.h` table that's a cache
  gets its own format number and is dropped when it changes (media/library.c CACHE_FORMAT, setting
  media.cache_format): CREATE TABLE IF NOT EXISTS never changes a table that's there.
- **Widgets**: `widget(c, b, WIDGET_HOME)` - a card for the home dashboard (walls are screens of blocks,
  not widgets, since 0.6.0). Must cope with `c->me == NULL` and with a display account. **Agenda**:
  `agenda(c, from, to, out)` + `agenda_add()` puts timed things into shared calendars (book: bookings,
  plan: calendar events, and - only when out->todos, the calendar feed - to-dos with a day); read them with `agenda_collect(c, from, to, except_id, &ag)` - a module that
  shows its own items itself reads with except = its id (plan's calendar does).
- **Sidebar foot**: `side(c, b)` renders a small block above the profile in the sidebar (phones: under
  the sections on /more). Every page calls it - cached numbers only, no disk/network work (media's
  storage meter refreshes its cache from `tick` every 30 s).
- **Health**: `health(c, h)` reports checks for the admin System page with `health_add(h, HEALTH_OK|
  INFO|WARN|BAD, title, detail, todo, link)` (plain words; a flag, variable or command goes in `health_tech(h,
  text)`, folded under "Technical details" - issue #62) and getting-started steps with `health_step(h, done, title,
  detail, link)`. A disk the module keeps things on: `health_disk(h, &(HealthDisk){what, path, free,
  total, data_disk, want, why, todo, link})` - never a disk row of its own: System makes one per disk.
  Called for admins only (also on the home page): cached facts only. `system` never links other
  modules - only this hook.
- **Admin-only module**: `.flags = MOD_ADMIN` hides it from non-admins everywhere (nav, /more, pages,
  http hooks, side(), widgets, agenda items, the /api action list); its actions still check `is_admin`.
  `MOD_NO_CHILD` / `MOD_NO_GUEST` do the same for children / guests (module_visible).
- **Someone leaves** (issue #45): `int forget(Forget *f)` - hand on or drop what's theirs (f->person): with
  f->keep only what's ahead of them (their history stays), else everything; with !f->apply only count and
  `forget_note(f, "3 bookings still to come are cancelled")` - the remove dialog shows it first. The core
  runs every hook, then removes sessions, prefs and the row (person_forget). Keep it to your own tables.
- **Blocks for screens** (the TV view, wall tablets - modules/screens arranges them): `.blocks` =
  `{id "<mod>.<name>", title, kinds SCREEN_TV|SCREEN_WALL, render, choices}`, terminated by {0}.
  `bool render(c, b, kind, arg)` draws one row's content WITHOUT its heading (false = nothing to show:
  the row is left out); `choices()` (optional) lists what the block can show (e.g. one series) for the
  builder's pick, passed back as `arg`. Look one up with `block_find(id, &owner)` - also how one module
  shows another's row without linking it (Watch's home renders "tv.onnow" with kind 0 = an ordinary
  page: plain links instead of TV-mode ones).
- **Time zone** (zone.h, issue #39): a module that keeps a DAY as a local midnight (an all-day thing, a due
  day) or times people typed implements `rezone(Rezone *z)`: in the days pass (z->days) rezone_day() its
  days, else rezone_time() what was typed before z->before (rezone_span does both ends of a span); with
  z->apply false only count and rezone_note() a preview. Moments the clock recorded (created, done_at,
  last_seen) never move.
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
  web/app.css unprefixed: .sr-only (read, not shown) .card .btn .chip .chips .chip-r .check .seg .tick .quick .preview .rows
  .row .badge .hued .field .field-row .stack .modal* .empty .toolbar .notice .muted .small .center.
- **Forms through the helpers** (`ui_form` / `ui_form_confirm` / `ui_act`, closed with `ui_form_end`):
  a display account's pages render in view-only mode (core render_app -> ui_view_only), where a form
  whose action isn't `.display` is cut out whole (issue #54: the hall tablet showed quick-add boxes,
  ticks and x buttons that only answered "view-only"). A form written by hand isn't covered. A dialog
  (ui_modal) in which a form was cut goes whole too, and links that OPEN a dialog ("?edit=", "?new=",
  "?res=", a booking or event row) ask `ui_can("mod.verb")` first - plain text for a display account
  (issue #96). core/app_screen_test.c renders every module page (and the dialog addresses) as a display
  account and fails on any other form, any dialog, and any link with such a parameter.
- **Rules**: no `#ifdef _WIN32` (use plat.h/lan.h); don't block the event loop (threads for disk
  scans/streaming, results handed back on `tick`); check `app_testing()` before opening sockets or
  starting threads; don't edit core/ or web/ for one module's needs — ask for a core change instead.
  A thread you start frees only what it allocated itself: the job the event loop made for it, the bytes
  `conn_detach` handed over and anything else from the loop go back with `free_on_loop()` /
  `buf_free_on_loop()` (core/base.h; docs/decisions/0010 - musl kept every block freed the other way).
- **Tests**: `modules/<id>/<id>_test.c` with tests/harness.h: `th_boot()`, `th_person()`,
  `th_act(person, "mod.verb", "a=1&b=2")`, `th_page(person, "/mod?x=1")`, `th_halt()`.
  Anonymous = person 0 → expect the sign-in page (`data-mod="auth"`). For sign-in tests call
  `auth_configure("salt", 1000)` (low iterations keep tests fast). Tests run in English (no pref, no
  browser language); `th_untranslated(person, url, allow, &report)` renders a page in all three
  languages and counts what reads the same in Swedish or Spanish (text that missed its T()) - every
  module has a `*_i18n_test.c` that keeps its pages at 0. A word translated to itself ("System" in Swedish)
  and a test-only module's words (ids "zz...": its title, block titles, block choices) don't count. Tests
  share a process with whatever the runner split puts before them - CI's runners use 4 processes, and
  tools/ci/local.sh (./build.ps1 ci) does too (HEMMA_TEST_JOBS); a test-only module stays registered.
- **Languages** (issue #46; core/i18n.h): every text people read goes through `T("English")` where it's
  said now (pages, ctx_ok/ctx_fail, health texts, labels, title=/aria-label=/placeholder, data-confirm)
  or `N_("English")` where it's said later or to others: app_notify*/activity_add formats (the core
  translates each screen's toast and each reader's Latest line itself), Module.title and Block titles
  (shown with T()), static tables (T(table[i]) where shown). Plurals TN(n, "%d video", "%d videos").
  Whole sentences only, never glued fragments; inline tags stay inside the sentence (printed with %r,
  or the translation used as H()'s format) and the translations keep them and the specifiers in order.
  Values (%s) are never translated - pick a whole format per case. Text KEPT for later (a database row,
  a worker thread's message, a setting) is i18n_pack'ed and i18n_unpack'ed when shown (book_news,
  Session.agent, the rescue/restore reports). The language is per request and per thread: tick(),
  start() and threads speak English - never cache translated text. Logs, the command line, /api and
  docs stay English. Dates via weekday_name/month_name/ui_day (never English arrays of your own);
  a decimal comma in sv/es: i18n_decimal(). Translations: `modules/<id>/<id>_lang.c` (more files for a
  big module, each TRANSLATIONS name unique), informal du/tú, sentence case, "Hemma" untranslated.
  WHICH LANGUAGE (core/app.c ctx_lang): the person's pref PREF_LANG ("en"/"sv"/"es"; "" = automatic -
  Your account's Language card = action people.lang (field lang, optional id: anyone else's for admins
  only; open to children and guests: ACT_ALL), an admin's person dialog (people.save `lang`) for anyone
  incl. display accounts), else the
  browser's: the cookie hemma_lang (the signed-out pages' language links -> GET /lang?to=xx&next=/path,
  303 back; to=auto forgets it), else Accept-Language, else English. Ctx.lang / Ctx.dev_lang (the
  browser's, -1 = none of ours); a WebSocket screen keeps its browser's from the upgrade request. The
  page carries `<html lang>` and `#app[data-lang]`; wire.js reloads a page whose morph brings another
  data-lang (its own words come in `<script type="application/json" id="wire-tr">`, ui.c WIRE_TEXTS).
  The calendar feed: the person's pref, else the calendar app's Accept-Language.

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
data-muted. The SOUND (volume + muted) is the viewer's per device: wire.js keeps it in localStorage
"hemma.sound" on every volumechange and gives it to each new video in media() (soundApply) - it reset to
full with every page (owner, 2026-10-17); onair()'s forced autoplay mute is flagged (_forcedMute) and not
saved. Parts [data-p]: play back fwd seek at dur mute vol subs snd; [data-snd="<n>" data-src="<address>"] = the LANGUAGE
menu (2.2, UiPlayer.sounds): sndChoose sets the video's src to that address (the same video with sound n alone - stream.c's
view), data-resume = where it was, plays on if it played, sends live media.sound; each menu is its button's own (the
click handler takes part.parentNode's [role=menu]); [data-sub="<key>"] menu items (1.2: core/ui.h
UiSub {key, lang, label, pic, on}; + data-subpic for a picture track) - subPick/subHold show the track whose id is
"sub-<key>", else the first in the key's language (before its first dot), else none; v._sub = the pick, carried
into the next episode in the same element; subWatch re-applies a pick on textTracks 'change' (Chrome turns the
first track on by itself when the next episode's tracks move during the morph); subTicks ticks by the showing
track's id (tests/browser/subtitles.spec.mjs). [data-sub][data-subpic="<cue list>"] = a PICTURE track (Blu-ray
PGS / DVD VobSub, no <track>): wire.js draws it - the list fetched once per video element, cue n's PNG = the list's
address with ".json" -> "/<n>.png", query kept - in a .pl-subpic layer ([data-client], aria-hidden: pictures of
words) over the SHOWN picture (object-fit contain of videoWidth x videoHeight), the list's frame w x h mapped onto
it (MakeMKV's 720x576 on NTSC too); lifted above the bar while it's up (a new cue jumps, the bar slides it);
looked up on timeupdate/seeked and every frame while playing, the next PNG fetched ahead (spShow/spDraw/spPlace/
spLoop). Never beside a text track (subHold: the same key, else the first of its language in the menu's order); an
item arriving aria-checked is drawn from load (plWire); the pick goes on to the next episode and into the browser's
own player (nativePlayer copies _sub). A pick made by the viewer (the menu, or C - subChoose) goes to the server
too (live media.sub; kept while the socket is down: subLater) - see "FOLLOWING THE VIEWER" under media/subs.c.
C (1.3) = subToggle: off, or back on - the last shown in this player (v._subPrev), else [data-subpref], else the
first. Keys (registered BEFORE [data-arrows]): Space/K, Left/Right ±10 s, M, F, C; on a computer they stay the video's even on a focused bar button; UiPlayer.tv puts
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
`[data-caps="<type>|<type>|..."]` (2.5, media/devices.c: answered ONCE A LOAD - canPlayType per type, y/m/n - as the
live media.caps; the list and its meaning are the server's; wire.js capsTell) ·
`[data-upnext="S"]` (the page's "next episode" card for its video[data-next]: unhidden in the last S
seconds, `[data-count]` = seconds left, `[data-hide]` puts it away for that video, a link in it reports
the video watched BEFORE the nav goes; an [autofocus] in it takes the focus from the video) ·
`form[data-a]` · `[data-live]` · `[data-auto]` (submit on change) · `input[data-show="id"]` (#id shows the field's value: a slider's number, also after
every morph) · `input[type=file][data-shrink="N"]` (issue
#47: the form carries the picked picture shrunk to N px on its longest side, as a JPEG data: URL under 600 KB -
quality then size step down; refused with a toast while the socket is down; app.c json_value keeps every socket
field whole now - a 4 KB buffer used to cut them) ·
`[data-esc]` (Escape) · `[data-back]` (TV mode's Back - issue #57: remotes rarely send Escape. The
remote's Back key clicks it: Backspace outside text fields, BrowserBack, GoBack, XF86Back, keyCodes 8,
166, 461 (LG), 10009 (Samsung); full screen is left first, an open menu closed. Clicking it (the key,
Escape, OK on it) goes through goBack() and never grows the history (issue #98): each pushed entry keeps
history.state.prev; Back to that page = history.back(), else a nav with pop:1 (the server answers
replaceState). A TV browser also going back by itself: a second popstate within 1.5 s is undone
(history.forward) - can't undo a step out of the document. A display's "/" is its screen: Ctx.display_home,
so media_row_back() gives "/" (what the TV's address bar says). The arrows reach it: Up on a player's
picture, Left on a live picture, Left/Up past the edge of a [data-arrows] area (toBack); from it Down/
Right go back where the focus came from, else [autofocus]) · `[data-key="/"]` (shortcut) · `[data-static]` (never morphed; `data-static="v"`:
skipped while v stays, a new v morphs the SAME element in place and settles it again - how a player
keeps full screen into the next episode/programme/channel: stable ids media-video / tv-video) ·
`a[data-native]` (full load) · `form[data-keep]` (don't reset) · `form[data-confirm]` (asked before
any submit, socket or not) · `form[data-upload="/base"]` (files: POST base/plan, then one PUT per file
to base/<batch>/<i>?at=<offset>, resuming from GET base/<batch>/<i>; a todo entry `[i, at, end]` sends
only the bytes before `end`; `{i, f}` = convert file f here first (web/conv.js Workers, half the cores,
max 8, as far ahead as the room slider allows), POST ?size= / ?original=1, then PUT the result as entry i;
`{i, f, subs: 1}` = only read f's subtitles and send those (1.3); ONE file at a time, whatever is ready first
(finished copies, then plain entries in order - 1.3); the `ram` box keeps copies in memory, sent in pieces
(?at=&end=; form[data-part] = the piece size, tests only); a `conv`
form field is dropped when the browser can't run WebAssembly; the server renders progress; with a
`[data-drop]` area inside, the form keeps its chosen files itself - dropped anywhere on the page, folders
walked with their paths, each choice adding, the area `[data-over]` while dragging, `[data-drop-clear]`
empties - and `form[data-preview="<live action>"]` sends them as [[path,size]] for the server to draw what
Start will do) ·
`audio[data-book]` + `[data-listen="<audio id>"]` (THE LISTENER, 1.6 - an audiobook, media/books.c; held by
tests/browser audiobook.spec.mjs): media() hands it to book(). Its place is always in the WHOLE book (data-length
seconds): a book in parts (data-parts [[address, start, length]...]) plays them in the same element, on 'ended' the
next part (partGo: src swapped, play on), and every seek (the bar, back 15 / forward 30, chapters, the lock screen's
seekto) maps book time to a part (bookSeek). Chapters = the page's #media-chapters a[data-at] (aria-current on the one
playing, its title in [data-b=chapter] and on the lock screen); a click on one seeks there and plays. Speed: the
[data-speed] menu, localStorage "hemma.speed", put back on every new part/play. Sleep timer: [data-sleep] minutes or
"ch" (the end of the chapter playing), checked on timeupdate + a 1 s interval, the button counting down. REPORTS
(bookTell): media.progress (live) every 15 s of the book, at a pause, at the very end (ended=1); when the socket is
down - and always when the page goes out of sight (visibilitychange hidden, pagehide: a phone being locked) - a plain
`fetch('/a/media.progress', {keepalive, redirect: 'manual'})` (app.c's POST path: same origin, the cookie). THE
LOCK SCREEN (owner: "listen to it while the phone is locked"): the Media Session API (not a secure-context API -
works on plain http), set while it plays: metadata {title = the chapter, artist = data-author, album = data-title,
artwork = data-art}, handlers play/pause/stop/seekbackward 15/seekforward 30/seekto/previoustrack/nexttrack
(chapters), setPositionState every 4 s. Taken off the page (another page came), it lets go of the lock screen, its
handlers (sessionDrop) and the stream. Keys: Space/K, Left -15, Right +30, Escape closes a menu. IN THE ANDROID APP
(2.0): window.HemmaApp (apps/android MainActivity.Bridge) - a WebView shows no Media Session on the lock screen, so
appTell() sends HemmaApp.media(JSON {state playing|paused|none, title, artist, album, art (absolute), duration,
position, rate}) at every play/pause/chapter change and every 4 s (sessionPlace), "none" from sessionDrop; the app's
buttons call window.hemmaAction(what, seconds) -> lockActs (the same handlers as the Media Session's: play pause stop
seekbackward seekforward seekto previoustrack nexttrack). lockBook replaces the old ms._book. Held by
audiobook.spec.mjs ("in the Android app") ·
LETTING GO (1.6; owner, 2026-10-28: a paused South Park episode played on from Watch and Add videos with the
keyboard's play key / the OS's media controls): every wired video/audio is in `wired`; settle() ends with letGo() -
one no longer on the page is paused if it plays (its pause handler reports, then drops src + load()), else drops
its src at once; a book also leaves the Media Session. A paused element kept its file before: only removal while
playing (the browser's own pause) let go. load() without a src sets the time to 0 and fires timeupdate: a report
after letting go would say "0" - bookTell says nothing without a src (a video's tell needs m.duration, NaN then).
Held by player.spec.mjs and audiobook.spec.mjs ·
`video[data-resume|data-report|data-next]` (THE PLAYER'S RULES - issue #49, each held by tests/browser:
player.spec.mjs / live.spec.mjs: data-resume = where it starts once loaded, never in its last 5 s; data-report
= media.progress (live) every 15 s of the video's own time, at a pause and at the end; ended -> data-next; the
"next episode" card ([data-upnext="S"]) in the last S seconds of a video longer than 3 x S; live = data-resume
+ the time since the page came, a pause/seek/speed change snaps back. The server keeps its half: media.c
media_resume_at() (over 10 s, not done, not in the last 5 s) and media_file_src_at() puts that, or a
channel's offset, in the address as #t=<s> - a browser without wire.js starts there too) · `[data-start="id"]` (open a horizontal scroller at that
child, once per URL — e.g. today's column on a phone) · `[data-fit="<id>"]` (a box of cards, its
children, that must fit its CSS-bounded height - a wall tablet nobody scrolls. A bounded box (overflow
hidden) is LAID OUT by wire.js ([data-fit-laid], app.css: absolute cards; issue #97 - CSS columns keep
the order, so cards went while a third of the screen was empty): each card in order into the shortest
column, n = min([data-fit-cols], cards shown, width / (--fit-col 300px + --fit-gap 18px)); the CSS
columns are the no-JS fallback, a phone keeps them. 1. the longest cards give up their last `li` rows
down to FIT_ROWS (4), then FIT_MIN (2), 2. whole cards go from the end, 2b. cards come back where room
is left (down to FIT_MIN), 3. a lone card shrinks further, then rows come back - fairly: the card showing
the fewest rows first; "+N more" (.fit-more,
or the server's `[data-fit-more="N"]` row gets the total), `[data-fit-keep]` rows stay, a
`[data-fit-head]` goes when nothing under it is left; per-card rects, since a card too tall for its CSS
column doesn't show in the box's scroll size; `[data-fit-cut]` is display:none !important and animations
are off inside; again after every morph and resize; the element with that id names the cards left out).
Phones (≤600 px): the tab bar shows the first 4 sections + "More" (`/more`, core/ui.c `TAB_SLOTS`);
wide grids scroll sideways with a sticky first column (hours, channels). Check new pages at
390×844 (chrome-devtools `emulate` "390x844x3,mobile,touch"): no sideways page scroll.
ACCESSIBILITY (issues #63-#70; keep new pages like this): text at 4.5:1 or more (--muted is #6f675b in the
light theme - don't fade text with opacity on top of it), tap targets 44 px (button.tick has a ::before tap
area), an icon-only button named with its item ("Remove Bananas", aria-label or .sr-only text), an avatar on
its own named (ui_avatar), motion that one @media (prefers-reduced-motion) block in app.css can stop (it
does: every animation/transition .01 ms). wire.js: DIALOGS (dialogs() after every morph and at load: a
.modal[role=dialog] takes the focus - first field, else its h2 - Tab/Shift+Tab go round inside it, the rest
of #app is inert (re-set after each morph), and closing it puts the focus back on its opener, else
keepSettle); FOCUS AFTER A MORPH (keepSpot/keepSettle: the focused control's item = nearest ancestor with an
id, outside dialogs and [data-arrows]; taken away -> the same item if still in its list, else the next, else
the previous, else the item wherever it went - ticking to-dos walks down the list); `[data-say]` (a line a
screen reader hears when it changes - the quick bars' previews: once typing pauses 0.7 s its spoken() text,
aria-hidden parts left out and "·" as a comma, goes into ONE polite live region #wire-say, which announce()
also uses for the "Next episode" card); in-page links (`href="#main"`) move the focus to their target;
the PLAYER says its state (Play/Pause label, mute aria-pressed, seek aria-valuetext "0:12 of 22:35") and its
bar stays while the keyboard is on it (lastKey < 30 s; not a TV's [data-arrows] bar - a remote's focus rests
on Play while watching).
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
  newcomer takes the place of a slot holder - an idle keep-alive connection, a slow reader (queued output
  that hasn't moved SLOW_CHUNK = 64 KB in T_SLOW = 10 s: Conn.t_moved/moved, issue #32 - a few bytes now and
  then used to keep a slot for good), else a half-sent request - from the address holding the most
  (server.c slot_holder; screens/WebSockets never; `server_set_slow_after` in tests); strangers before the
  household (Conn.known = a signed-in request was served on it: app.c on_http sets it); and with none of
  those, the longest-held BUSY connection of the address holding the most strangers' (busy_holder, at least
  BUSY_FLOOR = 4 of them; issue #91: reading just over 6.4 KB/s held a slot for good) - never a known one.
  A shared 32 MB budget
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
  title). Rows show "1–2" (.media-ep-nn) and series totals count every episode. One episode (or film) in
  two files with the same number: library.c part_cmp orders them by media_name_part ("Pt. 2", "CD2", "del 2";
  none = 0), then the path (1.4; owner, 2026-10-27: "any part 2 to be after the first part" - by name alone
  "Dark Side Pt. 2 [..." came before "Dark Side [...", and Next episode played it first). Names are parsed
  again from the path at every start (never cached), so parser changes reach existing libraries.
  SPECIALS (1.3; owner, 2026-10-24: "everything that is not in a series increment ... goes inside like a specials
  part of the series", "last, apart"): season 0 = Specials - S00Exx, a Specials/Season 0 folder, and (library.c
  loose_specials, at index time) a MOVIE/OTHER video lying right in a series' own show folder (media_names_show;
  never a category folder or the library folder; never an extra by name or a copy): an unnumbered season-0
  episode, numbered by season_pass after the series' highest S00, ep_title = its file's own name parsed alone
  minus the show's name in front. Extras folders stay left out, disc bonus clips stay MF_BONUS extras. ORDER:
  season_order() puts season 0 after every season (episode_cmp, extra_cmp) - tabs, TV rows, Live TV channels,
  zips follow; "Start watching" = S01E01. APART: media_neighbour never crosses between season 0 and the seasons
  (no "Next episode" card from the last season into them); media_next_up goes by the seasons' episodes only
  (after the last season: back to the first; a series of only specials walks them). MediaSeries.specials;
  "%d seasons" = media_seasons(s) (without Specials); nseasons still counts the tabs. The tab says "Specials".
  A RANGE OF SEASONS is a tag (names.c is_season_tok, 1.3): "S01-S31", "S1-12" (one token: tokenize doesn't
  split on '-'), "Season 1-2", "Seasons 1-9" (SEASONS_WORDS) - "The.Simpsons.S01-S31.1080p" names "The
  Simpsons"; such a folder is never a season folder.
  Movies vs series: episode markers / season folders -> series; "Title (Year)" -> movie; folders named
  Series/Serier/TV Shows/Anime or Movies/Films/Filmer or Home videos/Family/DCIM lean the guess. 2.4 (owner, 2026-10-30:
  a "Disney" folder of 54 films named without years came out as other videos; "the year of a movie to be almost
  mandatory"): library.c makes ANOTHER video of 60+ minutes a film (not an extra, a disc rip's title or a play-all; home
  videos folders still win - at the scan and from the cache); names.c drops a collection's ORDER NUMBER from a film's title
  ("1  Aladdin" two spaces, "1. The Rescuers", "1.5. The Lion King (Hakuna Matata)", "2 - Title" -> MediaName.order in
  tenths) but keeps one with one plain space ("101 Dalmatians", "300 Rise of an Empire"), glued to a dot ("2.Fast.2.Furious")
  or of four digits ("2001 A Space Odyssey"); episode rules come first. The upload preview gives every film without a year
  a box `y.<upload_show_key(title as found)>` (1880..next year; from fields and the JSON plan) -> <movies>/<Title (Year)>/
  <its own name>, and while some are empty the form's data-confirm asks "N films have no year. Upload them anyway?"; a top
  folder of the chosen files with other videos gets "These are films" (`films.<hex of the folder>` = 1: sorted as films).
  Library settings (admins): "Films without a year (N)", up to 20 with Manage's Fix title.
- Streaming is direct play (bytes as stored, Range requests, worker thread per stream, `sendfile`
  zero-copy on Linux). Browsers can't decode MPEG-2 (the owner's DVD seasons) and Chrome/Firefox
  not AC-3, E-AC-3 or DTS (they play the picture silently; DTS nothing plays): that's what modules/av is for (owner wants it
  built in C, not ffmpeg - ffmpeg is at most a comparison on a developer's machine, never shipped).
  E-AC-3 seen in real files: "DDP5.1" x265 releases (5.1, 640 kbit/s, 6 blocks, the exponent strategy
  table, no AHT/SPX; ~420x real time per PC core) and Dolby's own streaming encoders (SPX + AHT at
  128-256 kbit/s). The probe's why says "Dolby sound: Safari/Edge only - silent in Chrome and Firefox"
  and wins ties with the picture's; media_probe_judge() re-judges cached probes at every load.
  In the test show, seasons 1, 2, 3, 5 = hard-telecined film (IVTC -> 23.976p), season 6 = progressive
  29.97p (pass). The DVD rips: 720×480, 29.97 fps, interlaced (top field first), AC-3 2.0 192 kbit/s 48 kHz
  (Dolby Surround flagged; some episodes carry a 2nd commentary track) in laced 32 ms frames;
  video tracks not flagged default. All 183 tracks decode exactly (ac3: ~1,400× real time on a
  PC core, ~96× under pisim model 3). Target for "convert once": MP4 with H.264 + AAC-LC (the
  one audio codec every browser, iPhone, TV and Chromecast plays). MPEG-2 decode: ~1,100 fps on one PC core, ~59 fps
  under pisim model 3 (2× real time, 3 MB) — live decode fits a Pi 3, live H.264 *encode* doesn't.
- Uploads (admins, /media/upload): written as `<name>.hemma-part` (MEDIA_PART_SUFFIX - Hemma's own:
  issue #89, a torrent's `.part` was listed and deleted), renamed when complete, fsynced; resumable;
  refused up front when the disk lacks room (+1 GB reserve, media/room.c - conversions keep it too). On
  the Pi the service needs write access to the media folders (install-pi.sh takes them as arguments).
  An upload's half-sent file nobody touched for MEDIA_PART_DAYS (7) is listed by the scan (Scan/
  MediaIndex .parts, MEDIA_LEFT_MAX 100 shown, counts + bytes for all) on Library settings ("Unfinished
  uploads", admins): media.parts_remove re-checks each (still there, still untouched, not upload_claims
  of the video's name) before deleting - never on its own (issue #40); more than 100: the toast says the
  rest show after the scan. Plain `.part` files (older versions' too) are never listed.
- Library folders by one tap (issue #27): media.c suggestions() = HEMMA_MEDIA (env, ";" between; until in the
  library) + - only while the library has no folders - plat_mounts() (Linux /proc/self/mounts under /mnt and /media,
  file systems that hold files; macOS /Volumes; Windows other fixed/removable drives), each once, existing only, with
  its size; Watch's empty page shows them as "Use <folder>" buttons, Library settings as + chips -> media.roots_add
  (admin; save_roots() shared with media.roots).
- Rescan: admins, plus members an admin ticks on Library settings (setting `media.rescanners` = member ids;
  only the admin-only action `media.rescanners` writes it; display accounts, admins and removed people
  never count). Owner's rule (2026-10-12).
- A video's id hashes its library folder's path (media_make_id), so the same disk mounted elsewhere, a
  rename or a reorganised folder gives it a new id (issues #36, #93). Progress rows keep (identify(), on
  every report and for rows whose video is there at each scan) `place` = hash64 of the path below the
  library folder, `fp` = MediaFile.fp (size + first and last 64 KB hashed: fingerprint() in the scan,
  read once per new/changed file, cached in media_file.fp - a column added in place by ALTER, no new
  cache format), `ident` = media_ident() (film: title + year + disc + track; episode: show + season +
  episode range; folded) and `label` (media_label: "Film (2021)", "Show S01E04"). After each scan
  (swap_in -> watch_follow(new, old)) rows whose video is gone move (media_watch_move) to the ONE video
  with the same place, else content, else ident (only_one: never one of two), and nothing moves while a
  library folder is AWAY: missing, or empty where the old index had files (an unplugged disk's mount
  point; g_away). At start media_lib_start -> watch_fill_from_cache() fills rows lacking place/ident/label
  from media_file by id, in ANY root (the next scan's cache_store forgets unconfigured roots' rows).
  What still finds no video: media_history_lost() -> Library settings "Watch history without its video"
  (media.history_forget, admins, not while away) + media's health INFO "Watch history". Ids, addresses
  and series ids (from titles) stay as they were.
- Left out of the library (issues #28, #94): an Extras/Trailers/Featurettes folder's videos
  (MEDIA_LEFT_EXTRA, no length asked); names ENDING in trailer/sample/featurette/... after "-", ".", "_" or
  " - " (MediaName.named; a plain space doesn't count: "The Interview"), or the whole name, under
  MEDIA_EXTRA_LONG_S (40 min, MEDIA_LEFT_NAMED); names STARTING "sample" under MEDIA_SAMPLE_LONG_S (10 min,
  MediaName.sample, MEDIA_LEFT_SAMPLE). The scan probes the named ones; long, or a length the probe can't
  read (TS, WMV, MPEG-PS), = a real video, named again with the name rules off (media_parse_name_long;
  named_but_real(); cache_load applies the same from the cached duration; MediaLeft.duration_ms saves the
  next scan a probe - left-outs aren't in the SQLite cache, so once per start). A name with an episode code
  is never an extra by its name. "sample" is no release tag ("The Sample" keeps its title). Listed on
  Library settings ("Left out", a reason each; MediaIndex .left); the scan's log line counts them.
- Disk room (issues #29, #95): media/health.c reports each library folder's disk with health_disk()
  (no row of its own): wants CLAMP(biggest video + MEDIA_RESERVE, 2 GB, 20 GB) "to upload or convert a
  video as big as its biggest (X)"; data_disk when the data folder shares it (RootFacts.data_disk: same
  total, free within 64 MB). The System page (disk_rows) makes ONE "Disk space" row per disk: the data's
  (DATA_WANT 2 GB, DATA_BAD 300 MB) merged with modules' data_disk ones, others grouped by size + free;
  "<free> free on the disk with Hemma's data and the library (N% used). The library wants X free ...",
  sizes with one decimal under 100 GB (room_words) so the two numbers can't round alike.

## Where things stand

What each version changed is [CHANGELOG.md](CHANGELOG.md); why the big choices are what they are is
[docs/decisions](docs/decisions/README.md) (0001-0010); how it fits together for humans is
[ARCHITECTURE.md](ARCHITECTURE.md) and [CONTRIBUTING.md](CONTRIBUTING.md). This file keeps the working rules.
(Issue #51, 2026-10-21: the dated Status diary that lived here - every version's work, review passes -
is the CHANGELOG's now.) 1.0.0 (2026-10-21) = the outside reviews' issues (#3-#103) and the planned
household features (#27 #39 #41-#47 #71-#74) all done, plus languages (#46), the browser's side tested
(#49), reviews, releases and codec scope settled (#48 #50), docs for contributors (#51). 1.1.0 (branch
feat/library, owner's own wish, no issue) = Watch as a library: downloads (a video, a season or series as one
.zip, giving way to watching), uploads you can see before sending (drop area, the server's preview, Hemma
sorting into Series/Movies, "Add videos" on Watch, "Add episodes" on a series), and Manage (remove an episode,
season or series; fix a film's title or an episode's number on disk; what takes room). 1.1.1 / 1.1.2 = fixes (an MPEG-2 second field never
predicts from itself; Dolby at 44.1/32 kHz). 1.2.0 (branch fix/subtitles-addition; owner, 2026-10-23: "all the shows
that have subs to be able to give subs") = subtitles from every MKV (refused ones too, WebVTT, 32 tracks), DVD/Blu-ray
picture subtitles (.idx/.sub, .sup) drawn by the player, the server taking subtitles out of the library by itself
(media/subtake.c), same-language tracks kept apart (keys, labels), uploads carrying them all. 1.3.0 (branch
feat/faster-uploads; owner, 2026-10-23: "make the 12mbit/s limit ... be always at that limit") = uploads send whatever
is ready (no waiting on the list's order), conv.js reads ahead, copies optionally kept in memory (a big checkbox; sent
in pieces), and what stays behind still gives the subtitles the library lacks (subtitles-only entries; sidecars and
lone subtitle files go next to the library's video); then (owner, 2026-10-24: "faster branch + extra file support") DTS
sound (av/dts.c) and subtitles following the viewer through a series, C toggling them; admins renaming a series
(its folders; it keeps its id), and a release folder's season range ("S01-S31") no part of a series' name. 1.4.0
(branch feat/add-categories-to-series-and-movies; owner, 2026-10-27: "increase the searching and usage of the series
and movies") = categories (media/cats.c: by hand - members and admins -, from .nfo files, and - an admin's box, off by
default - looked up online, media/lookup.c + core/fetch.c, with "Update categories" by a pasted TVmaze/IMDb id), the
lists' chips, order and Not started/Started, a row per category on Watch and for the TV, search by every word in any
order, grouped; and an upload that waits out a dropped network. 1.5.0 (branch feat/upload-fixes; owner, 2026-10-27: "it
sort of spams between texts ... make it more modern") = an upload's card that says what it's doing - a state, three
numbers in fixed places, and what's under way (Saved / Uploading / Ready / Processing / Subtitles), the furthest along
on top - and a subtitles-only entry that stays done. 1.5.1 (branch fix/memory-leak-issue; owner, 2026-10-27: "we
should find it out what caused it") = memory on the Pi no longer grows with uploads and streams: worker threads hand
the event loop's memory back (free_on_loop, decision 0010) - musl had kept every 2 KB job a worker freed - and the
Network page's eviction no longer loses a list. 1.6.0 (branch feat/audio-book-support; owner, 2026-10-28: "audio books
library ... listen to it while the phone is locked"; a minor version: nothing breaks) = audiobooks: .m4b anywhere and
folders of .mp3/.m4a under an Audiobooks folder (av/audiotags.c reads their tags, chapters and covers), Watch's
Audiobooks tab, a book's page with a listening player (15/30 s, chapters, speed, sleep timer, parts in one element),
the lock screen through the Media Session API, progress that survives a locked phone, downloads and uploads of books.
2.0.0 (branch p-o-c/app; owner, 2026-10-28: "a web-app pretty much, only allow local connection ... it tries to find the
local-network of the hemma application", "a 2.0 deploy ... a nice new soft-launch where an app is available") = the
Hemma app for Android (apps/android, docs/decisions/0011, docs/android.md): Hemma's pages in a window of its own, found on
the home Wi-Fi by itself (GET /hemma.json, Avahi's _hemma._tcp), home network only, the lock screen's controls for
audiobooks, served by Hemma itself (Your account -> Get the app for Android). An iPhone app needs a Mac with Xcode and
Apple's paid programme to share it (a free Apple ID installs on one's own iPhone for 7 days): not now.
2.1.0 (branch fix/make-another-converter-possible; owner, 2026-10-29: "Its picture is AV1, which the converter can't
change yet" - "make this viable also as a convert") = AV1 MKVs converted (the picture copied into an MP4 as HEVC is,
Dolby/DTS beside it made AAC), in the uploading browser and by Convert; and (owner: "Noticed you are in the web,
download the app instead!") Home offers the app to a browser on Android - people.c's widget (widget_order 2), shown
while the app is built in, the session's agent says Android and isn't the app, the person has no session from the app
and hasn't pressed Not now (people.app_hint -> pref people.app_hint "off"); never to a display account. CI's actions on
Node 24 (mlugg/setup-zig has none yet). A developer's own build signs the app with their dev key, which won't install
over the release-signed app: a build for a Hemma whose phones have the released app leaves it out (HEMMA_SKIP_MODULES=app).
2.2.0 (branch video/language-track-update; owner, 2026-10-30: "I want hemma to be not 'language locked'"; a film with
Russian x4, Ukrainian and English sound came out Russian only) = SOUND LANGUAGES: the converter keeps every sound track
(av/conv.c, AV_CONV_MAX_AUDIO 8: video, then the sounds in the MKV's order, each mdhd language + hdlr name, only the
default tkhd-enabled, runs of 48 samples with 2+ sounds), the library knows a video's sound tracks (media_sounds), the
server sends each viewer an MP4 with only their sound (mp4view.c media_view: a new ftyp + moov in memory, the file's
mdat as it is; /media/file/<id>?a=<n>, built on the stream thread, cached) - so every browser, TV and the app plays it
(only Safari switches between several tracks itself) -, Hemma asks a viewer's sound language once (media.c
media_sound_question, pref media.soundlang: "" = ask, "-" = each video's own), and the player has a Language menu
(core/ui UiPlayer.sounds, wire.js sndChoose - it also sets the new key "<id>-a<n>" itself, so the morph after the
pick leaves the player be; pref media.snd.<series id | video id> = "<lang>#<n>": that very track in that file, else
the language).
Also (owner: "open the app instead!"): people.c's Home card for someone WITH an app session in an Android browser says
"Open the app instead!" - an `intent://open/#Intent;scheme=hemma;package=...;S.browser_fallback_url=<the apk on Ctx.host>`
link; the app's VIEW + BROWSABLE filter takes hemma://open/<path> (MainActivity.pageOf: a path only, opened on the
Hemma it knows). An app from before 2.2 has no such filter: Chrome then goes to the fallback (the download).
2.3.0 (branch app/offline-running-support; owner, 2026-10-30: "a 'offline' section in the app, where you can download
videos/audiobooks and play them offline from the app"; WHO: "Grown-ups and children" - members, admins and children, a
child only what's open to children; guests and display accounts never - unlike 1.1's downloads, grown-ups only) = OFFLINE
IN THE APP. Hemma's side (media/offline.c): media_offline_ok(c); GET /media/offline/<id>.json = a video's or a book's
manifest {"v":1,"kind":"video"|"book","id","title","series","code","poster","duration_ms","resume_s","files":[{"url",
"bytes"}],"subs":[{"url","lang","label","on"}],"chapters":[{"t","title"}],"author","narrator"} (a video's file =
media_file_src: the viewer's sound; text subtitles only, as .vtt; a book's parts + chapters in whole-book seconds) and
/media/offline/season/<series>-<n>.json = {"kind":"list","title","items":[manifest paths]}; 403 text for guests and
screens, 404 for what a child may not see; Cache-Control no-store. Links only in an app session (session_in_app), never
on TV pages: hemma-app:save?m=<manifest, %-encoded> ("Save for offline" after Download on a watch page and a book page;
"Save season for offline" in the season toolbar - which children now get in the app, holding only that) and
hemma-app:offline (Watch's header; people.c's "The Hemma app" card in the app: "Saved for offline"). The app's side:
apps/android (see there). Progress comes back through the plain POST /a/media.progress (same origin, the cookie).
2.4.0 (branch fix/movie-identifier) = FILMS GET THEIR YEAR: the upload preview's Year boxes (almost mandatory: Start asks
first), "These are films" for a folder, order numbers out of titles, a video of an hour or more a film, and Library
settings' "Films without a year" (see "Media notes").
2.5.0 (branch fix/wording-patch) = MP3 SOUND COPIED into the converter's MP4s (35 of the Disney films had H.264 + MP3 and went up
as they were), and the "browsers play it silently" note only for TrueHD; and (owner: "teach the converter to work with dobby")
MP4/MOV INPUT (DuckTales: 101 MP4s, H.264 + AC-3, silent in Chrome): av/mkv_mp4.h reads them for the converter (stsd ->
Matroska codec ids, ctts + elst times, tx3g -> S_TEXT/UTF8, chapter/text tracks out, fragmented refused); AvConvJob.as_is =
an MP4 that plays everywhere (H.264 / hvc1 / AV1 + AAC or MP3) - nothing written, conv_run 3, conv.js {plays}, wire.js
original=1&plays=1 -> UpFile.plays: sent as it is quietly (no card notice); the plan sends every MP4/M4V/MOV through the
converter (preview badge "Converted here if needed", not counted as "to convert"); convert_need: MP4/MOV + Dolby/DTS =
transcode, + AAC/MP3 = none. Copied pictures' times rounded to the nearest tick (at_tick), not each length down.
names.c tok_year: a run of years ("1987-1990", "1988-89") is its first year - a pack folder "DUCKTALES (1987-1990) -
The Complete ..." had come out "DUCKTALES (1987-1990) - The" and split the show (seasons whose folders said no more took it).
Then (owner: "we don't have to give an image to a show / movie. It should automatically fetch") PICTURES FROM THE INTERNET
(media/lookup.c, decision 0012: TVmaze for series, Wikipedia for films through the computer's curl - core plat_run +
fetch_https), OPUS SOUND COPIED ("Its sound is Opus, which the converter can't change yet"), and a rename no longer looks a
title up before the library has read its new name. And (owner: "check the issue with the videos that are not playable
everywhere"; "Both, devices first") WHAT YOUR DEVICES PLAY (media/devices.c): browsers report canPlayType for a list of
types, Library settings counts what plays on every device of the household, lists them and the reasons; HEVC -> H.264
(an HEVC decoder) only for what turns out to fail on one of them. Then (owner, 2026-10-31: "the videos on hemma that are
not converted", "fix it in this branch") XVID AND DIVX CONVERTED (av/mpeg4.c - see the av layout; the Pi's ten that played
nowhere: Solsidan S06E03-04 + S07) and TWO FILES OF ONE NAME ARE ONE VIDEO (owner: "How come the doublicate deletion cannot
pickup the arhur episodes?" - library.c same_stem/lone_version: "x.mkv" + "x.mp4" in one folder lasting as long
(media_copy_lasts) pair like a copy made here: the more compatible (media_compat_cost) becomes the MF_WEBCOPY the other
streams through, Manage/the watch page say "Another version", Keep only the most compatible version deletes the other).

Keep new work within what's been measured:
- pisim model 3 (≈Pi 3 CPU, 512 MB): idle 0.2 wake-ups/s; 8 viewers × 8 Mbit/s ≈10% of the core; ceiling
  ≈1.5 Gbit/s before the CPU runs out - the network is the limit (Pi 3B 100 Mbit, 3B+ ~300 Mbit over USB 2
  shared with the disk; Pi 4/5 gigabit for 4K remuxes); peak memory 8.6 MB; a 3.6 GB upload at disk speed in
  9 MB RAM. A real Pi 3B+ over Wi-Fi: 8 viewers × 8 Mbit/s with 0 errors, 91 Mbit/s for one stream.
- The converter: a 22-minute DVD episode ~282 s on one PC core natively, ~374 s as WebAssembly; video
  bit-identical native vs wasm, AAC differs only by float rounding; copies 190-480 MB per episode vs ~1 GB
  originals; a 7-season show (171 files) converted with 0 failures; E-AC-3 ~420× real time per PC core.
- The program: ~3.3 MB for the Pi (0.25.0 added ~300 KB of translations); the suite ~10 s on a 20-thread PC.

Owner decisions not in docs/decisions: the Network page's changes (device edits, owners, Wake-on-LAN, the
presence window) are admin-only - members only look (2026-10-11); the sign-in page keeps its "Welcome back"
guess; the licence is Apache 2.0 and there's no "Support the project" section for now (2026-10-14); the
project lives at https://github.com/kloparn/Hemma (the guides clone it); no Windows download until hemma.exe
is signed or cleared with Microsoft (see Owner preferences).

Next ideas: builder-style UI helpers / less raw HTML (owner asked), calmer home cards (hide empty ones), touch
on the wall (ticking the shopping list - needs .display actions), more wall blocks (weather is out: LAN only),
MKV→MP4 remux on the fly for Safari/iPhone (planned as 1.2, owner 2026-10-21: a "virtual MP4" over the MKV,
Dolby passed through for Apple devices; live AC-3 -> AAC after), "Open in VLC" links, router integration, DLNA, API tokens for
scripts, "tune in" on the TV's browser via a QR code, more video formats as files need them (HEVC decoding,
AV1 decoding for older Apple devices, interlaced XviD and GMC, Vorbis sound, TrueHD, AVI files - each with its fuzz
seed: docs/decisions/0006).
