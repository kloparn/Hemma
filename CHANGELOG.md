# Changelog

What changed in each version of Hemma. `hemma --version` says which one you run (and the
commit it was built from). Downloads, from 0.14.0 on:
[github.com/kloparn/Hemma/releases](https://github.com/kloparn/Hemma/releases).

## 0.25.1 (2026-10-21)

The player and the upload page are tested in a real browser now, and the video code is fuzzed every
night.

### Fixed

- **Live TV and "continue watching" without JavaScript:** a channel started its programme from the
  beginning instead of where it is now, and a video didn't resume, in a browser that runs no
  JavaScript (or before Hemma's script arrived). The page now puts the starting point in the
  video's address, so the browser starts there by itself.
- A video no longer offers to resume in its last 5 seconds (it would end at once) - the server keeps
  that rule now, not only the player.

### For developers

- **Browser tests** (`tests/browser`, CI only): Hemma's pages in Google Chrome against a real Hemma
  with real (tiny) videos made by its own encoders - resuming, progress, the next episode, Live TV's
  rules with and without JavaScript, an upload, the sign-in languages - and eslint over `web/*.js`.
- **Nightly fuzzing** (`.github/workflows/fuzz.yml`): about three hours every night over the whole
  converter and, much faster, the MKV reader, the Dolby decoder and the MPEG-2 decoder on their own,
  each night going on from what the last one found.
- Local CI runs the suite in four processes, as GitHub does, and fuzzes any of the four targets
  (`./build.ps1 ci fuzz 300 ac3`).

## 0.25.0 (2026-10-21)

Hemma speaks English, Swedish and Spanish.

### New

- **Three languages:** every page, button, message, toast and check in Hemma is in English,
  Swedish (svenska) or Spanish (español). It's all built into the program - no internet, no
  translation service.
- **The browser's language by itself:** Hemma speaks what your phone or computer is set to, and
  English when that's none of the three.
- **Or your own choice:** **Household → Your account → Language** (Automatic, English, Svenska,
  Español). It follows you to every device you sign in on; children and guests choose too. An admin
  picks the language of a TV or wall tablet in its dialog under **Household**.
- **The sign-in pages** have *English · Svenska · Español* at the bottom, for a browser that asks for
  something else.
- **Quick text understands Spanish:** *"coche mañana de 14 a 17"*, *"sauna esta noche a las 19"*,
  *"aspirar todos los martes"*, *"cena el 5 de noviembre a las 20"* - beside English and Swedish.
- **A starter answers to its name in every language:** the "Car" books as *bil* or *coche* too.
- **Dates, days and sizes** follow the language: "mån 5 okt", "lun 5 oct", "1,3 TB ledigt".
- **What others did** (the Latest feed, toasts, booking news) reaches each person in their own
  language, and the calendar on your phone comes in yours.

### Changed

- On phones, a long button label wraps instead of pushing the page sideways.
- A few English texts were made whole sentences or proper plurals on the way ("of 1 file", not
  "of 1 files"); the card link "All" reads "See all".
- The server's log, the command line and the guides stay in English.

## 0.24.0 (2026-10-20)

Dolby Digital Plus sound plays everywhere, and a video's own subtitles come along.

### Fixed

- **Videos with Dolby Digital Plus sound ("DDP5.1", E-AC-3) played silently** in Chrome and
  Firefox, and an upload sent them unconverted: the converter only knew Dolby Digital (AC-3).
  It now decodes Dolby Digital Plus too - in Hemma's own C, like the rest of the converter, on
  the server and in the uploading browser - and makes the usual copy with AAC sound, which every
  browser plays (the picture is copied as it is: quick). For videos already in the library, the
  video's page says why it's silent and offers **Fix the sound**; **Watch → Library settings**
  converts every such video at once.

### New

- **Subtitles inside an MKV come along:** when Hemma converts a video, on the server or while
  uploading, its text subtitles (SRT, ASS/SSA) are saved as `.srt` files next to it - named by
  language ("Movie.en.srt", "Movie.sv.forced.srt") - and the player offers them. Never over a
  file that's there. (Picture subtitles - PGS, VobSub - can't be turned into text yet.)
- **The upload says out loud what went up unconverted**, and why ("Its sound is DTS, which the
  converter can't change yet"), instead of only in the folded list - such a video may play
  without sound, or not at all, in some browsers. The converter's reasons are in plain words
  now, not codec ids.
- The video's page tells Dolby sound apart: "Dolby sound: Safari/Edge only - silent in Chrome
  and Firefox" (videos already in the library get the new words too).

### For developers

- modules/av/ac3.c decodes E-AC-3 (ETSI TS 102 366 Annex E): its header and audio frame, 1/2/3/6
  blocks a frame, spectral extension, the adaptive hybrid transform (vector and gain-adaptive
  quantisation; ac3_aht.h holds the spec's vector tables - FFmpeg's hebap-4 table lacks the
  spec's first row), mixing metadata. Only independent substream 0 is decoded (AC3_SKIP for the
  rest); enhanced coupling and half rates are AC3_ERR_UNSUPPORTED. Checked: real DDP5.1 episodes
  parse bit-exactly and match FFmpeg but for dither; Dolby-made test clips with coupling,
  spectral extension and AHT match FFmpeg but for noise; the test encoder (ac3_test.c) writes
  E-AC-3 with every quantiser and the decoder rebuilds exactly what it sent.
- av_convert: A_EAC3 sound, frames of any length; `want_subs` hands back text subtitle tracks as
  SRT (AvConvJob.subs, av_convert_free); errors in words (codec_words). convert_save_sub /
  media_sub_sidecar name and save them; the upload API's `?sub=1` takes the browser's
  (web/conv.js posts them, wire.js sends them after the copy).
- media_probe_judge(): cached probes get today's verdict at load. tools/ac3dump.c reads E-AC-3
  and writes raw floats (`f32`) for comparisons; its raw-stream reader no longer re-reads 8 MB a
  frame.

## 0.23.0 (2026-10-20)

A ready-made Raspberry Pi image: write it, plug the Pi in, open http://hemma.local (#71).

### New

- **`hemma-<version>-pi.img.xz`** with every release: Raspberry Pi OS Lite (64-bit: Pi 3, 4, 5,
  Zero 2) with Hemma ready to go. Write it to a memory card or USB stick with Raspberry Pi Imager
  (**Use custom**), put it in the Pi, plug it in, and open `http://hemma.local` a few minutes
  later. No terminal, no SSH, no installer to run.
- **Made on your Pi, never in the image:** the password secret, the one-time setup code and the
  SSH host keys are created at the first start; there are no users or passwords in it, and SSH
  stays off unless you turn it on in Imager. Imager's settings (Wi-Fi, user, name, time zone)
  still apply - the machine is named `hemma` only if you didn't choose a name.
- **Security updates by themselves:** the image brings `unattended-upgrades` (installed at the
  first start, no internet needed for that), and Hemma updates from System → Updates.
- **Only the newest release has an image.** When a new release comes out, the image goes from
  the older ones (their program downloads stay): nobody should start a new Pi from an old
  Raspberry Pi OS and an old Hemma.

### For developers

- tools/image/build.sh (no root: debugfs writes into the image's ext4 partition) + debs.awk (the
  .debs the image lacks, from Debian's index verified with the keyring inside the image) +
  firstboot.sh / hemma-firstboot.service (runs the bundled install-pi.sh at the first start).
  release.yml builds it with every release (bundle.sh with HEMMA_IMAGE=1) and the release's
  SHA256SUMS covers it; after publishing, it deletes the `.img.xz` from every older release
  (`gh release delete-asset`) - only when the new one has its image.

## 0.22.0 (2026-10-20)

The movie disk from the browser: plug it in, press a button (#74).

### New

- **"New disk found".** Plug a USB disk into the Pi: within seconds **Watch → Library
  settings** shows it under **Disks** - its name, file system and size - and the System page
  says so too. **Use it for movies** does the rest: the Pi mounts it at `/mnt/<its name>` now
  and at every start (`nofail`: a missing disk never stops the Pi starting), Hemma may save
  uploads and converted copies there, and it becomes a library folder. Hemma restarts once to
  reach it. Nothing on the disk changes; ext4, exFAT, NTFS and FAT disks work as they are.
- **Format for Hemma** - only for a disk with nothing on it at all, and only after typing
  ERASE: one ext4 file system named `movies`, then used as above. A disk with any file system
  on it is never formatted from Hemma.
- **Missing disk.** Unplugged, the System page says "The movie disk … is missing" instead of
  showing an empty library; plugged back in, Hemma has it mounted again by itself. **Forget**
  stops the Pi mounting a disk you no longer use (its files stay).
- **`sudo hemma-disks`** does the same from a terminal: list, `use`, `format`, `forget`.
- **Needs an installer run** of this version (the update from System → Updates or `sudo
  hemma-upgrade` is one): it brings the disk helper.

### For developers

- modules/media/drives.c: unprivileged detection (`/sys/block`, udev's `/run/udev/data`,
  `/proc/self/mountinfo` - `media_drives_scan`, portable file reading, tested on a made-up
  machine); the Disks card, the format dialog (`?format=<disk>`), the first-run offer, checks;
  actions `media.drive_use/_format/_forget/_seen`; `media_root_add/_drop`.
- deploy/hemma-disks (sh, root) + hemma-disks.service/.path: requests `use <UUID>`, `format
  <disk> <bytes>`, `forget <UUID>` + a nonce in `/var/lib/hemma/disks/request`; answers in
  `/var/lib/hemma-disks` (result, disks). Refuses the system's disk, non-USB disks, anything
  mounted or in `/etc/fstab` by hand, and formatting anything with a file system. Writes
  `/etc/fstab` (copied first, the new line checked with `findmnt --verify`, put back if the
  mount fails) and `hemma.service.d/disks.conf` (never the installer's `media.conf`); mounts
  through `systemctl start <unit>.mount`, so the mount is the system's, not the service's.

## 0.21.1 (2026-10-20)

The first release with signed downloads - and so the first that updates itself (#73).

0.21.0 was never published: its release stopped, as it should, because the project's signing
key wasn't in place yet. This version carries the key, and everything 0.21.0 brought:

- **`sudo hemma-upgrade`** on a Pi or any Linux installed with `install-pi.sh`: the newest
  release, checked against the project's signature, with a way back if the new one doesn't start.
- **System → Updates**: Check now (or once a day, if you turn it on), Install, and Install from
  a file for a Pi without internet.
- **Needs one installer run** from this download (`sudo sh install-pi.sh ./hemma-pi64`): it
  brings `hemma-upgrade`. From then on, updates come from Hemma. More under 0.21.0 below.

### For developers

- The project's release key is in `RELEASE_KEYS` (core/release.c); CI signs with the matching
  GitHub secret `HEMMA_SIGNING_KEY`.

## 0.21.0 (2026-10-20)

Updates with one command or one tap - checked against the project's signature, with a way back
(#73).

### New

- **`sudo hemma-upgrade`** on a Pi (or any Linux installed with `install-pi.sh`) updates Hemma
  to the newest release: `--check` only looks, `--dry-run` downloads and checks without
  installing, `--file hemma-pi64.tar.gz` installs a download you brought on a USB stick.
- **System → Updates.** The version you run; **Check now** (or **Check once a day** - off until
  you turn it on: then github.com is asked which version is newest, nothing else); **Install**
  when a newer one is out, with a link to what's new. No internet on the Pi? **Install from a
  file**: choose the download on any computer. The System page's checks say when a new version
  is out, when an update went in, and when one didn't.
- **Only signed releases.** Every release is signed now (`SHA256SUMS.sig`, Ed25519). The key's
  public half is built into Hemma, and the Hemma you have checks a download before anything of
  it is installed: one that was changed, or isn't from the project, is refused and nothing
  changes.
- **A way back by itself.** An update keeps the program it replaces and a copy of the database;
  if the new Hemma doesn't answer within a minute, both are put back and the old one starts
  again. The Updates tab says so - nothing is lost.
- **The operating system's updates** show there too: whether Debian installs its security
  updates by itself (`unattended-upgrades`), and when it last looked.
- **Needs one installer run** (`sudo sh install-pi.sh ./hemma-pi64` from this download): it
  brings `hemma-upgrade` and its service. From then on, updates come from Hemma.

### For developers

- `core/ed25519.c` (Ed25519 after TweetNaCl, its constants worked out at the first use, RFC 8032
  vectors + Node's as tests), SHA-512 in `core/codec.c`, `core/release.c`: `hemma
  --gen-signing-key FILE`, `--sign FILE` (key in `$HEMMA_SIGNING_KEY`; refuses a key that isn't
  built in), `--verify-release DIR [--partial]`; the public keys in `RELEASE_KEYS`.
- release.yml signs every SHA256SUMS with the GitHub secret `HEMMA_SIGNING_KEY` (bundle.sh's
  `HEMMA_SIGNER`) and refuses to publish without it; each download holds a `VERSION` file.
- deploy/hemma-upgrade (sh, root) + hemma-update.service/.path: Hemma writes "check" / "install
  X.Y.Z" / "install-file" + a nonce into `/var/lib/hemma/update/request`; the helper answers in
  `/var/lib/hemma-update` (latest, result), reads Hemma's folder only as Hemma's user, unpacks
  nothing it hasn't looked over, runs the download's own installer (`HEMMA_INSTALL_WAIT=60`).
- modules/system/update.c: the tab, the actions `system.update_check/_auto/_install/_seen`, the
  upload (`form[data-upload="/system/update"]`: one PUT, no resuming); `plat_bundle()`.

## 0.20.0 (2026-10-20)

Easier setup: no paths typed from memory, and the setup code without a terminal (#27, #72).

### New

- **Your media folders, one tap.** The installer now notes the folders you give it
  (`sudo sh install-pi.sh ./hemma-pi64 /mnt/media`) in the settings file as `HEMMA_MEDIA`. Until
  they're in the library, Watch offers them as buttons - **Use /mnt/media** - and Library settings
  as **+** buttons under the folder box. While the library has no folders yet, the disks Hemma can
  see are offered too, with their size: under `/mnt` and `/media` on Linux, `/Volumes` on a Mac,
  the other drives on Windows. Typing a folder still works.
- **The setup code on the memory card.** On a Raspberry Pi, the welcome page's code is also in
  `HEMMA-SETUP.txt` on the card: shut the Pi down, put the card in any computer and open the file.
  A screen plugged into the Pi shows it on the login screen. As soon as the admin exists, both say
  the code is used up. The welcome page says where to look. (A small helper, `hemma-setup`, writes
  them - root's places; it takes nothing from Hemma but "the code" or "done".) **Needs an installer
  run** (`sudo sh install-pi.sh ./hemma-pi64`): copying just the program leaves the helper out.

### For developers

- `HEMMA_MEDIA` (";" between folders); `plat_mounts()` / `plat_parse_mounts()` (core/plat);
  media's `suggestions()` + action `media.roots_add`.
- `hemma --setup-note [--boot DIR] [--issue FILE]` (auth_setup_note_cli); Hemma keeps
  `<data>/hemma.setup` ("open XXXX-XXXX" / "done", auth_setup_state_write at start and after the
  welcome page); deploy/hemma-setup.service (root oneshot, at boot) + hemma-setup.path
  (PathModified on that file); both in the Pi/Linux downloads.

## 0.19.0 (2026-10-19)

Uploads that convert on your computer no longer fill its disk.

### Fixes

- **A long upload filled the sending computer's disk, then stopped with "The connection keeps
  dropping".** Every copy the browser made stayed with it after it was sent - the browser only lets
  go of such things when the page's memory is next tidied, which a page this small hardly ever
  needs - so after a few dozen 1080p episodes it held 28 GB of them on its own disk (in its profile,
  on the system drive). When that was full the next copy broke, and the page read the failed send
  as a dropped connection, again and again. Now each copy stays inside the converter that made it,
  is sent from there, and goes the moment the server has it: checked with the same four episodes,
  the browser held 700 MB after the old way's upload had finished, and was back where it started
  with the new one. Should the browser ever lose a copy anyway, the upload stops and says so ("is
  its disk full?") instead of retrying forever.

### New

- **Room on this device for copies waiting to be sent**, a slider on the upload form (2-32 GB, 8 to
  start with; remembered per person). Copies are made only so far ahead of the one being sent:
  more room lets more slow conversions - DVD rips take minutes each - run side by side (still at
  most half the computer's cores). It used to be up to nine copies, however big.

### For developers

- web/conv.js keeps its finished copy (`{size}` out) and sends it itself on `{put, at}` (answers
  `{status, body}`, `{lost}`); wire.js's converter holds such workers in `ready` and ends them
  (`drop`) once sent or skipped; `put(url, src, ...)` sends a File or through `src.send`. Jobs start
  while the originals of those running and waiting add up to the form's `room` at most (one always
  may). New generic hook `input[data-show="id"]`: #id shows the field's value, also after a morph.

## 0.18.0 (2026-10-19)

People and privacy: children and guests (#44), someone moving out (#45), and what everyone else sees
of you (#42).

### New

- **Children.** A new kind of sign-in, *Child* (with or without a sign-in of their own). A child sees
  their own to-dos, adds and ticks off their own, uses the shopping list and the board (changing
  only their own notes), looks at the calendar, and books only the things open to children - for
  themselves, cancelling or moving only their own bookings. Anything else says *Ask a grown-up to
  do that*. No Network page.
- **What children may book**: each thing has **Children may book it** and **Guests may book it**
  (an admin's). A new TV room, sauna and office start open to children; a guest room, laundry and
  sauna to guests; a car to neither. **Things you already have start open to neither** - tick them.
- **Not for children.** An admin marks a folder under **Watch → Folders** (Library settings lists
  them, and opens one again). Its videos stay out of a child's Watch: lists, search, Continue
  watching, the watch page and the video's address itself. A series with an episode in such a
  folder goes whole, and so does a Live TV channel that would air something from one. The same
  holds on a TV while a child is picked under *Who's watching?*.
- **Guests.** *Guest* sign-ins have a **last day**: after it their devices are signed out and
  signing in says *Your sign-in has ended* and who to ask for more days. Until then a guest books
  what's open to guests, uses the shopping list and the board, and watches; they don't see who's
  home, the household's news, calendar, to-dos, screens or Network page, and under Household only
  their own account. No recovery codes for guests (an admin sets a new password).
- **Someone moving out.** **Remove** now opens a dialog that says what will happen before it does,
  two ways: **they moved out - keep their history** (bookings that happened, notes, Latest and what
  they watched stay under their name, "Eva (moved out)"; their bookings to come are cancelled,
  repeating ones stop, open to-dos go to anyone or to the next in line, devices are nobody's, the
  sign-in goes), or **erase everything about them** (the rest goes too; what they added for
  everyone stays, without their name). People who moved out are listed under **Moved out**, with
  **Back in the household** and **Erase**.
- **Privacy**, under Household → Your account: how the wall tablet shows you (with home or away,
  just your name, or not at all), whether others see that you have Hemma open and where, whether
  the TV's *Who's watching?* lists you, and whether your phone and computer may tell Hemma you're
  home.
- **Your devices** on the Network page: the ones an admin marked as yours - rename one, or say it
  isn't yours.
- The demo household's Maja and Leo are children. The README shows a child's phone.

### Changed

- **The TV's *Who's watching?*** lists whoever chose to be on it: members and children until they
  untick it, admins and guests once they tick it (the box is under Screens and under Privacy).
- **Latest** on Home leaves out what happened in sections you don't have (a child doesn't see the
  Network page's news).
- The JSON API is for members and admins only (children and guests: 403, like display accounts).

### Fixes

- **A thing open until midnight lost its last hours when its dialog was saved.** The dialog showed
  *Until 22:00* for one open until 24:00 (the sauna, say), and saving it - to tick *Children may
  book it*, for instance - made it close at 22:00. Midnight now shows as 00:00 and stays midnight.
- Moving a booking without saying who it's for kept it theirs (it used to become the mover's).

### For developers

- Roles `ROLE_CHILD` (3) and `ROLE_GUEST` (4); `Person.until` (a sign-in's end) and `Person.left` (moved
  out: the row stays for names, out of `person_count`/`person_at`/`person_real`/`person_find`);
  `person_signin_over()`, `person_label()` ("Eva (moved out)"), `is_child()`, `is_guest()`.
  `LOGIN_ENDED` from `auth_login` (only after the right password), the sign-in page's `e=5`.
- `Action.may` (`ACT_CHILD` | `ACT_GUEST`): actions children/guests may run, refused in the core
  otherwise like a display account's. `app_action_open(name, role)` replaces
  `app_action_display`; `ui_role(role)` replaces `ui_view_only` (forms and dialogs a role can't
  use are cut). `MOD_NO_CHILD` / `MOD_NO_GUEST` module flags (net: both; screens: guests).
- `Module.forget(Forget *)` and `person_forget()`: count and say first (`apply` false, `forget_note`),
  then hand on or drop (book, plan, board, media, net; the core: sessions, codes, prefs, TV picks,
  erased Latest lines, the row).
- Privacy prefs and `privacy_wall/_doing/_on_tv/_home()` in the core: `presence_screens/_page` say
  nothing of someone who opted out, `presence_set_home` ignores devices of someone who did.
  `PREF_ON_TV` moved to app.h.
- media/kids.c: table `media_no_kids` (library folder + folder below it), `media_lib_for(c)` (a child's
  copy of the index lists, remade when `media_lib_gen()` or the marks change), `media_kid()`,
  `media_kids_ok()`, `media_kids_series_ok()`; `media_recent()` takes the Ctx. Table
  `resource` gained `kids`, `guests`.

## 0.17.0 (2026-10-19)

Photos on notes - the last part of #47.

### New

- **A photo on a note.** The board's note box and a note's dialog have a **Photo** field: the school
  letter, the parking permit, a drawing for the fridge. The phone or computer shrinks the photo
  first (1600 pixels on its longest side, a few hundred KB), so it's quick to send and light on the
  Pi's disk, and leaves the rest of the photo's details (like where it was taken) behind. A photo
  alone will do as a note. It shows on the board, on the home card and on the wall; on the board it
  opens full size. A note's dialog can take the photo away or put another in; photos no note has
  any more are cleaned up (a deleted note's Undo still brings its photo back).

### Fixes

- **A long form field over the connection kept only its first 4 KB.** Nothing in Hemma sent one
  that long until photos, but it was cut without a word; it now arrives whole (the connection's own
  1 MB per message is the limit), and so does the JSON API's.

### For developers

- `input[type=file][data-shrink="N"]` (wire.js): the form sends the picked picture shrunk to N px, as a
  JPEG data: URL under 600 KB (quality, then size, step down until it fits), and refuses with a
  toast while the socket is down (a plain POST can't carry it). `base64_decode` in core/codec.
- Board photos are `<data>/board/<16 hex>.jpg` (checked to be a JPEG, its size read from the frame
  header so pages give width and height), served at `/board/photo/<name>` to the signed-in, cached
  for good; swept at the start and hourly once nothing has them for an hour.

## 0.16.0 (2026-10-19)

Chores that take turns, and bookings and events that repeat (#47, the first two of its three).

### New

- **Chores that take turns.** Name more than one person on a chore that repeats -
  *"vacuum every week @anna,@erik"* - and each time it's ticked off, the next one in line gets it,
  who hears so ("Your turn next: Vacuum · Fri"). The list says whose turn is next ("next: Erik"),
  and a turn can be swapped in the chore's dialog ("Swap: Erik does this one, Anna the next"). The
  dialog's **Taking turns** field sets the line by name; someone who moved out is skipped.
- **Repeating bookings.** *"laundry every tuesday 18-21"*, or **Repeat** in the booking dialog
  (every day, week, 2 weeks or month, until a day if you like). The next eight weeks are booked
  at once and later ones as the weeks go, so they show and block like any booking. One already
  taken is left out, and whoever it's for is told. A booking's dialog offers **Cancel this one**
  or **Stop repeating** (this one and every later one).
- **Repeating events.** *"football practice every wednesday 17:30-19"*, or **Repeat** in the
  event dialog: made a year ahead, so the wall, the calendar and your phone's calendar show them.
  One changes or goes on its own; **Delete this and later ones** stops the rest. Every month on
  the 31st falls on the last day of the shorter months.
- The demo household (`hemma --demo`) takes turns vacuuming, and has a football practice and a
  sauna that repeat.

### For developers

- A repeating booking or event is a series (`booking_series`, `plan_event_series`) that makes real
  rows ahead (`book_series_fill`, `plan_series_fill`: at the start, after creating one, and hourly
  from the tick); occurrence k is counted from the first, so a month on the 31st never drifts. A
  series' rows carry its id (`Booking.series`, `Event.series`). Taking turns lives on the chore:
  `Task.turns` (ids in order), `.turn` (whose place this time is) and `.swap` (who gave this one away).
- The time zone (#39) knows series: their first times move with the days or the typed times, and a
  row a series made at the start after a change counts as typed when its series was.

## 0.15.0 (2026-10-19)

Your calendar on your phone, and a change of time zone that no longer moves things.

### New

- **Your calendar on your phone** (#41). Under **Household → Your calendar on your phone**,
  everyone who signs in can make a link that their phone's own calendar subscribes to: their
  bookings, their to-dos with a day and the ones for anyone, and the household's events and
  theirs, from a month back to a year ahead. The phone keeps a copy for when it's out, and its
  reminders work as usual; it brings it up to date on the home Wi-Fi. An iPhone subscribes with
  one tap (or by scanning the code on a computer's screen), Android through the free ICSx⁵, a
  computer's calendar by the address. A tick adds everyone's bookings. **Make a new link** stops
  the old one at once, **Turn it off** stops it altogether. A to-do still open after its day
  shows on today, saying when it was due.
- **A new time zone keeps days on their days** (#39). A Pi left on UTC for a week and then set
  to Stockholm used to show a one-day party on two days, spill a guest-room booking into the
  next day and put a 14:00 dentist at 16:00. Now, at the next start, things that last whole
  days (all-day events, chores' days, whole-day bookings, a note's last day) keep their days by
  themselves. The times typed before are listed on the System page as they'd be if moved back
  ("Dentist · Tue 13 Oct 16:30 → 14:30"): **Move them back to the times typed** if the old zone
  was wrong, **Got it** if Hemma really moved.

### For developers

- **Module.rezone** (core/zone.c): at a start in another zone the core asks every module to move
  the days it keeps as local midnights (`rezone_day`, `rezone_span`), and, when an admin asks,
  the times typed before (`rezone_time`, previewed with `rezone_note`). The zone's detection moved
  from the System module to the core's start, before the modules start. plan, book and board
  take part.
- **The calendar feed** (core/feed.c): `/cal/<id>-<key>.ics`, the key HMAC(server secret,
  "cal:<id>:<n>") - nothing secret stored; pref `cal.feed` counts the links made (negative =
  off). Built on the shared agenda: `Agenda.todos` asks plan for to-dos as well. `Ctx.host` is the
  address the visitor opened Hemma by.
- Tests can run in another time zone (`plat_set_tz("UTC0")`, `"XXX-2"`) and open pages by an
  address (`app_test_ctx_host`); `days_of_date` / `date_of_days` in base.

## 0.14.0 (2026-10-19)

Downloads, and a demo to try Hemma before setting it up.

### New

- **Ready-made downloads** for a Raspberry Pi (64- and 32-bit), Linux and a Mac (Apple silicon
  and Intel), on the [releases page](https://github.com/kloparn/Hemma/releases). A Pi or Linux
  download holds the program, the installer and the service file, so installing on a Pi is now
  download, unpack, `sudo sh install-pi.sh ./hemma-pi64` - and updating is the same. Every
  download comes with checksums (`SHA256SUMS`) and has the in-browser video converter. No Windows
  download yet: Windows Defender's machine learning calls `hemma.exe` a trojan (a false
  positive), so Windows keeps building it from the source for now; the guide says what to do if
  Defender removes the program you built.
- **`hemma --demo`** starts a made-up household in a throwaway folder of its own: four people,
  things to book with bookings on them, chores, a shopping list, events and notes. The login
  shows in Hemma's window and on the sign-in page, and every page says it's a demo. It never
  touches real data, and the next `--demo` starts afresh.
- **"Try it on your laptop first"** in the README, and a "download it" way at the top of the Pi,
  Linux and Mac guides, with the one click-through an unsigned program needs on a Mac.

### For developers

- **The version lives in the [VERSION](VERSION) file** (`MAJOR.MINOR.PATCH`, semver.org), easy to
  find: the builds read it into `core/version.c` alone, and a test fails until this file has a
  section for it.
- **Merging a new version releases it**: every PR still raises the version, and when main gets
  one without a release, `.github/workflows/release.yml` runs the suite, builds the programs,
  packs the downloads (`tools/ci/bundle.sh`), tags the commit `vX.Y.Z` and publishes the
  release with that version's section of this file as its notes, plus the commits since the
  previous release (no section: no release). Run by hand, the workflow is a dry run. See
  [docs/development.md](docs/development.md#versions-and-releases).
- **Two sign-in tests no longer depend on the machine's speed.** They make hundreds of wrong
  sign-ins to fill the throttle tables, and on GitHub's slower Windows machine the hashing budget
  (the flood guard that answers "busy" past 4 s of password checks) ran out part-way. They now
  top it up between tries; the budget keeps its own test.

## 0.13.1 (2026-10-19)

Faster builds and tests for whoever works on Hemma, and one small upload fix.

### Fixes

- **An upload counts a file before it answers "done"**. A browser told a file had arrived could
  ask straight away and still read "0 of 2 files": someone else's new upload was then refused
  with the old count. Seen on a busy Raspberry Pi.

### For developers

- **`make` builds one object per source file**, so `make -j8` keeps every core busy and a change
  compiles again only the files it touches (each object knows the headers it read). A new commit
  recompiles one file: the version with its commit now lives in `core/version.c`.
- **The test suite runs in one process per core** (`make test`, `.\build.ps1 test`): about 5-10 s
  instead of a minute on a 20-thread PC. `build/test --shard K/N` runs a share of it;
  `make test T=lan` runs a filter in one process. Running tests in different company showed one
  that leaned on an earlier test: the sidebar's disk meter now measures afresh at every start.
- **CI's Linux jobs on your own machine** (`tools/ci/local.sh test|asan|fuzz`; on Windows
  `.\build.ps1 ci`, in WSL): the gcc build and suite, the suite under ASan + UBSan, and the
  converter's fuzzer, in seconds to a minute. GitHub CI compiles with every core too.

## 0.13.0 (2026-10-19)

Accessibility: keyboard, screen readers, contrast, motion.

### Changes

- **Dialogs keep the keyboard inside** (#63). Opening one moves the focus to its first field,
  or to its title. Tab and Shift+Tab go round inside it, and the page behind can't be reached.
  Closing it puts the focus back on the button that opened it. A screen reader names the
  dialog by its title.
- **The video player says what it's doing** (#64). Play reads "Pause" while playing, Mute
  says whether the sound is off, and the position reads "0:12 of 22:35". The control bar
  stays while the keyboard is on it, up to 30 seconds after the last key. On a TV it still
  hides after 3 seconds and comes back with any key. The "Next episode" card is read out
  once when it appears, and on a TV it takes the focus from the player's buttons too.
- **Add buttons have names on phones** (#65): "Add to-do", "Add items", "Add event".
- **Grey text is easier to read** (#66). The light theme's secondary text went from 4.1:1 to
  4.9:1 contrast. Done to-dos are no longer faded on top of being grey and crossed out. A
  booking's small text in the week view is 12 px at full colour, and past bookings stay
  readable.
- **Bigger tap targets in Plan** (#67). The done circle and the shopping ×
  look the same but answer to a 44 × 44 px tap, with room between the circle and the to-do's
  link.
- **"Skip to content"** (#68) is the first thing Tab reaches. After an action the focus
  stays where it was: ticking a to-do off moves it to the next to-do, not to the top of the
  page.
- **Things that were only shown are read out too** (#69):
  - The quick-add preview ("Sauna, Today 19:00–20:30. Free") and clash warnings are read
    out once typing pauses.
  - Who a to-do or an event is for, and who added a shopping item, are read out as well as
    shown as a picture.
  - Each note's Pin, Edit and Remove buttons, and each device's Wake up button, say which
    item they're for, and so do the Screens editor's Up, Down and Remove.
- **Less motion when asked** (#70). With "reduce motion" set on the device, fades, slides and
  pops are over at once. The sign-in, welcome and recovery-code pages have a main
  landmark like the rest.

## 0.12.0 (2026-10-18)

Forms and wording: errors where they belong, Undo, bookings that tell their owner, plain words.

### New

- **Errors show next to the field they're about** (#60). An error about one field ("Use at
  least 8 characters") stays under that field, which gets a red border, until you change it.
  A screen reader reads the error with the field. An error about the whole form shows above its
  buttons. Toasts are kept for the rest, and they move to the top of the screen when they would
  cover the field you're typing in.
- **Undo instead of "Are you sure?"** (#60). Removing a shopping item, clearing the basket,
  taking down a note, deleting a to-do or an event, and the × on a Continue watching card
  happen at once, and the toast offers **Undo** for 30 seconds. Notes, to-dos and events no
  longer ask first.
- **Someone else cancelling or moving your booking tells you** (#43). The person it was for, and
  whoever made it, get a toast and a line on their Bookings page and home card ("Adam moved
  your Laundry booking · now Sat 10:00–12:00 (was Fri 14:00–17:00)") until they press
  **Got it**. An admin can make a thing stricter: **only the person a booking is for, whoever
  made it, or an admin can cancel or move it**.
- **Quick booking knows other names** (#55). The starter things answer to their Swedish names
  too ("bastu ikväll 19" books the Sauna, "tvätt imorgon 19-20" the Laundry), and any thing
  can be given **Other names** in its settings. A word that names nothing gets "I don't know
  'boat'. You can book: Car, Laundry, Sauna."

### Changes

- **Members are told who to ask** (#58). The Watch card's "Set up" led members to a page
  they can't change. They now read "Ask Anna (admin) to add the movie folders" (the
  household's admins by name), on Live TV, the Watch page and Library settings too.
- **Plain words instead of developer ones** (#62):
  - The **Address** check: "Only this computer can open Hemma: phones and the TV can't reach
    it", with the `--host` details folded away.
  - The player shows **File details** ("MP4 file · H.264 video · AAC sound"), folded, instead of
    "MP4 · H264 · AAC".
  - Library settings' **What plays where** starts with "Most videos play straight away", with
    the formats folded away.
  - The **Network** page says "Connected over Wi-Fi" or "over a network cable" instead of
    "Interface en0".
  - The password secret is "the password secret (a line in the settings file)" on the
    System page, with `HEMMA_AUTH_SALT` under **Technical details**.

## 0.11.0 (2026-10-18)

Hardening follow-ups: the TV's picker, connection slots, typed names, the browser log.

### New

- **The TV's "Who's watching?" has a guard** (#90):
  - Admins aren't listed until they tick **Show me on the TV** under **Screens** (the new
    "You on the TV" card). The server refuses picking them otherwise. Whoever holds the TV's
    sign-in could see and clear the watch history of anyone it lists.
  - Anyone can set an optional 4-digit **PIN** there. The TV then asks for it when that
    person is picked. After five wrong guesses it waits 1, 2, 4… up to 15 minutes. The PIN
    is kept as an HMAC with the server's secret.
  - A TV's or wall tablet's sign-in now ends **90 days after signing in**, however much it's
    used, like an admin's after 30.

### Fixes

- **A full server makes room from busy strangers too** (#91). Reading just faster than the
  slow-reader rule (6.4 KB/s) kept a slot for good, so 13 addresses could hold all 400. Now,
  when nobody idle or slow can make room, a newcomer takes the place of the longest-held
  connection from the address holding the most. That applies only to connections with
  nothing signed in on them: the household's own are never pushed out that way, and
  strangers always go first.
- **What was typed into the name box stays out of the address** (#100). After a wrong
  password or recovery code, the address said `/login?u=<what you typed>`: into the
  browser's history, and over the LAN in plain http. That was a password, when it went into
  the wrong box. Hemma now keeps it for that device for a minute and puts it back in the box.
  The "Forgot your password?" link no longer carries it either.
- **A browser's left-out log lines are summed up when the hour ends** (#102), not only when
  that person's browser writes again: a TV that repeated one message 500 times and went
  quiet now says so. The "(no more from …)" note comes before the page address, which is
  shortened, so a long one can't push the note off the end of the line.

## 0.10.1 (2026-10-17)

Follow-ups for the library: unfinished uploads, left-out videos, disk room, watch history.

### Fixes

- **"Remove unfinished uploads" only removes Hemma's own** (#89). Uploads now write
  `<name>.hemma-part`. A torrent's or browser's unfinished `.part` file in a library folder is
  never listed or removed. Removing also really skips a file an upload is about to resume (the
  check never matched before), and says when more than the 100 listed wait.
  Half-sent files from older versions, still named `.part`, aren't listed any more: delete
  them yourself if you see any.
- **Films and episodes named like extras stay in** (#94):
  - "The Interview", "The Sample (2019)" and "Show S01E04 - Crime Scene" are videos again.
    A name counts as an extra only after a mark that sets one apart: "Film-trailer",
    "film.sample", "Film_featurette", "Film - Trailer".
  - A name with an episode code is never an extra (an Extras folder still makes one).
  - A video named like an extra stays in when it runs 40 minutes or more, or when Hemma
    can't tell how long it runs (TS, WMV and MPEG-PS recordings). Before, those were dropped
    as "under 10 minutes".
  - "Left out" gives each file its own reason, and the scan's log line says how many were
    left out.
- **One "Disk space" check per disk** (#95). The System page and the library said "OK" and
  "nearly full" about the same disk: two checks, two rules, two roundings. Now there's one
  row per disk, naming what's on it ("Hemma's data and the library"), with the numbers it
  compares: "11.8 GB free … the library wants 7.0 GB free to upload or convert a video as big
  as its biggest (6.0 GB)". The library now wants room for its biggest video plus the 1 GB
  reserve, which is what an upload or conversion takes, instead of two of them: 20 GB free
  no longer warns next to a 10 GB film. Modules report their disks with the new
  `health_disk()`.
- **Watch history finds its videos again** (#93):
  - A video that's gone hands its history to the one video at the same place below a
    library folder. Else to the one with the same content (its size and its first and last
    64 KB, read once per file and cached). Else to the one that is the same film or episode
    by name. Never to one of two.
  - A renamed file, a reorganised folder and a film replaced by a new copy keep their history.
  - A library folder that's suddenly empty (an unplugged disk's mount point) counts as away,
    so nothing moves to a copy on another disk.
  - On the first start after upgrading, history from before 0.7.1 learns where its video was
    from the library cache, also after a move.
  - History that still finds no video is named on Library settings and the System page, and
    can be forgotten.

## 0.10.0 (2026-10-17)

Hemma backs itself up every night, checks the copies, and can go back to one (#35).

### New

- **Nightly backups.** Around 03:00 Hemma copies its database, checks the copy (SQLite's
  integrity check, and that it's Hemma's data), and keeps the newest of the last 7 days. The
  first one is made a few minutes after a start; a computer that was off at night gets one
  soon after it's on again. They go to a hidden `.hemma-backups` folder in your first
  library folder on another disk than the database (with a USB disk, they survive a dead
  memory card), else to `<data>/backups`. An admin can choose another folder.
- **System → Backups**: the backups with their date and size, **Back up now**, **Download**
  and **Go back to this**. Going back checks the backup, keeps what's there now as
  `hemma.db.before-restore-<date>` (the newest three stay) and restarts Hemma with it. A
  Pi's service starts it again by itself; started by hand, Hemma says to restart it.
- **Download** gives one `.tar` file: the database and `hemma-secret.txt` with the password
  secret (`HEMMA_AUTH_SALT`) every password is made with, marked as secret. The nightly
  backups leave the secret out.
- **`hemma --restore FILE`** puts a backup (`.db`) or a download (`.tar`) back, for example
  on a new memory card. It checks the file first and says if the settings file needs the
  old secret. Hemma puts it in place at its next start.
- **System → Checks** says when the last backup was made, and warns when one failed, is
  more than two days old, or sits on the same disk as the data. It also says when a backup
  is waiting to be put back, and when one was.
- The Pi guide's "Backups" covers putting Hemma back on a new memory card: the secret, the
  same media folders, the library disk at the same path. It also covers `/api` for getting
  your data into other programs.

## 0.9.4 (2026-10-17)

A damaged database gives back more, and never starts Hemma as a new household.

### Fixes

- **A damaged database never starts Hemma as a new household** (#87). Damage at the start of the
  file used to rescue nothing: Hemma then showed "Welcome to Hemma" over an empty database while
  the household's data sat in the damaged copy. Now, when a rescue gets next to nothing back (no
  rows, none of the household's accounts, or under a tenth of what was there), Hemma changes
  nothing and doesn't start, and says why. After any rescue the welcome page stays closed until
  an admin has read about it. If no admin account came back, everyone gets a page saying what
  happened and where the way back is, and members who sign in see it too.
- **The rescue keeps much more** (#87, #88). It now uses SQLite's own recovery (what
  `sqlite3 .recover` does), page by page. Damage on a page that points to other pages no longer
  loses the rows under it: a 3,000-note test that kept 0 now keeps all 3,000. A broken file
  header or a file cut short is rescued too. Rows that can't be put back where they belong are
  kept in a `lost_and_found` table, not thrown away.
- **The rescue says what was kept, in the household's words** (#88): "2,944 of 3,000 rows were
  kept - notes on the board: 2,944 of 3,000". Hemma notes how many rows each kind of thing has
  (`hemma.db.counts` in the data folder) to compare with.
- **Damaged copies don't pile up** (#88). A copy that fails half-way (a full disk) is deleted,
  and only the newest three `hemma.db.damaged-…` copies are kept.
- **A start refused over the data is easy to see** (#92). The reason is in `hemma.log` too, and
  it names versions: "This data was last used by Hemma 1.2 … Install Hemma 1.2 or newer again,
  or put back the copy made before the change: hemma.db.before-format-3". Hemma then exits with
  its own code (78), and the Pi's service no longer restarts it every 2 seconds.
- **Going back to the version before is a swap** (#92): `hemma` ↔ `hemma.prev`, so the newer
  one stays there to come back to. The guides also say how to put back the database from before
  ([raspberry-pi.md](docs/raspberry-pi.md#going-back)). A new section, "A damaged database" in
  [troubleshooting.md](docs/troubleshooting.md#a-damaged-database), covers getting more back.
- **The installer copies the database** (#92) before the new program first opens it:
  `hemma.db.before-install-<when>`, the newest three kept.
- **A renamed field can't empty a column for older programs** (#92): a test fails when a
  `FIELD_WAS` has no `breaks_older` migration at or after its data format (now part of
  `FIELD_WAS`).
- SQLite 3.54.1 (was 3.54.0), with its recovery extension.

## 0.9.3 (2026-10-17)

### Fixes

- **The player remembers your volume.** It went back to full with every new video, the next
  episode, a channel or a reload. Each device now keeps the volume you set, and whether you muted,
  for every video it plays. A browser that won't start a channel with sound still starts it muted,
  but that isn't remembered as your choice.

## 0.9.2 (2026-10-17)

Follow-ups for the screens, the TV and the wall.

### Fixes

- **The wall uses its whole screen** (#97). Cards now go into the shortest column, so a
  gap in an earlier column gets used. A card gives up rows down to two ("+N more") before
  any card is left out, rows come back to the shortest cards first, and Notes can be
  shortened too. With a busy week, a 1024×768 tablet now shows 7 of 8 cards with the
  columns filled (it hid 3 with a third of the screen empty).
- **"Didn't fit" is for admins only**, and the wall's **Exit** is gone when it would just
  bring the tablet back to the same wall (#97).
- **A view-only screen gets no links to dialogs it can't use** (#96): no "Add a thing",
  "Book", "New event with details", no edit links, and bookings and events are plain rows.
  A dialog opened by its address anyway shows nothing instead of an empty box.
- **Back on the remote doesn't pile up history** (#98): it goes back through the browser's
  history when that's where Back leads, and replaces the page otherwise. A TV browser
  that also goes back by itself doesn't take you a step too far. On Live TV, Left on the
  picture reaches Back.
- **Screens shows an address a TV can use** (#99): `--host localhost` counts as this
  computer only and `--host *` as every address (they were shown as `http://localhost`
  and `http://*`). The `.local` name is only offered when this computer announces it
  (on Linux: when Avahi runs; a Mac and Windows always do).
- **The TV row is called "On TV"** (#101), with the channels that are playing first and
  those between programmes after them.
- **Calendar titles get room on a tablet** (#103): below 1200 px wide the month sits above
  the agenda, and a title that can't get 8em beside its time goes under it.

## 0.9.1 (2026-10-17)

Ready for newcomers: the README and the guides. Nothing in the program changed.

### Changes

- **Hemma has a license: Apache 2.0** (#26). Anyone may use, change and share it. Until
  now no license was granted at all.
- **Screenshots** in the README (#22): Home and Book on a phone, the TV, the wall tablet and
  Watch's posters, from a made-up household. The films are the Blender Foundation's open
  movies (CC BY), credited in `docs/img/CREDITS.md`.
- **The README reads in a newcomer's order** (#24): what Hemma is, the screenshots, what's
  inside, then installing. "Why C?" moved to `docs/development.md`.
- **"Is Hemma for you?"** (#25): when to pick Jellyfin or Plex, Home Assistant or Nextcloud
  instead, honest both ways.
- **About posters** (#23): Hemma never looks anything up online, so new videos show as
  coloured cards. The README says how to add a poster.

### Fixes

- **The install guides say where to get Hemma** (#20): `git clone
  https://github.com/kloparn/Hemma.git`, or Download ZIP. Every "TODO (owner)" note is
  gone: `docs/development.md` says how to report bugs and send changes, and the empty
  "Support the project" section went.
- **Chrome's warning before http sites** (#75): the README and the home network guide say
  who sees it. Chrome 154 and later warn only for public sites, so Hemma's IP address,
  `hemma` and `hemma.local` open as before.
- **troubleshooting.md** shows the start-up block without a version that goes stale (#30).

## 0.9.0 (2026-10-14)

The TV, on a remote.

### Changes

- **The remote's Back key goes back** (#57). In the TV view - a series, a video, a Live TV
  channel - Back on a Samsung or LG remote, BrowserBack and Backspace now do what Escape did.
  In full screen it leaves full screen first. The arrows reach the on-screen Back button too:
  Up from a playing video, or Left/Up past the end of the player's buttons.
- **Screens shows the address to type** (#61), big, with `http://` in front, and the
  `.local` name for phones and computers. A QR code next to it lets a tablet or phone open
  Hemma without typing (Hemma draws it itself).

### Fixes

- **A new channel starts at once** (#56). A channel on the half hour used to count from the
  last :00 or :30, so it could show colour bars - "back at 10:00" - for up to half an hour
  after you made it. Now its first programme starts the moment you make it, and the
  half-hour rhythm picks up with the second. Channels on the half hour may move their
  schedule by half an hour once, when you update.
- **On now says when a channel is between programmes**: the next programme's picture is
  dimmed and labelled "Up next · 10:00" instead of looking like it's playing.

## 0.8.0 (2026-10-14)

The wall, finished.

### Changes

- **The wall fits its screen** (#77). Cards below the edge of a tablet were simply never seen.
  Now the wall shortens its longest cards first ("+3 more"), leaves out whole cards only when
  nothing else helps - the last ones in your order - and gives room back where it can. The
  corner says which cards didn't fit, so you can reorder or remove them under Screens, Edit.
  Checked at 1024×768, 1280×800, a portrait tablet and a 1080p TV; a phone still scrolls.
- **A display account opens the wall** (#53) unless it's set to show a screen of its own -
  not "Good morning, Hallway tablet".
- **Nothing to tap that only says "view-only"** (#54). A display account's pages leave out
  the quick-add boxes, ticks, × buttons and edit links it can't use.

### Fixes

- **The wall only shows** (#80): bookings, Recently added and On now no longer open things on
  the tablet, to-dos get a dot instead of a tick circle, a booking ending another day says
  when ("until Tomorrow 02:00"), and Edit and Exit are easier to find.
- **"+N more"** under Bookings when more are coming than the card shows (#53).
- **Recently added on Home** shows a series' new episodes as one card, as Watch does (#59).

## 0.7.2 (2026-10-14)

### Fixes

- **Live TV shows the picture as it is.** The old-TV scan lines and darkened edges laid over
  every programme are gone - in the window and full screen. The brief static between
  channels and the channel number stay.

## 0.7.1 (2026-10-14)

The library keeps what it knows.

### Fixes

- **Watch history follows the videos** (#36). A video's place in Hemma came from its library
  folder's path, so the same disk mounted somewhere else - a new memory card, `/media/usb1`
  instead of `/mnt/media` - took everyone's Continue watching and watched ticks away. Now
  the history finds each video again by where it sits inside the library folder. It waits
  while a library folder is missing (a disk just unplugged), and never guesses between two
  copies of the same folders.
- **Unfinished uploads show up** (#40). An upload that was never finished left a file as big
  as a film behind, for good, and nothing in Hemma showed it. Library settings now lists
  those nobody has touched for a week, with their size, and **Remove them**.
- **Nothing is left out without a word** (#28). Trailers, extras and samples stay out of the
  library, and Library settings now lists them and why. A video whose name starts with
  "Sample" but runs 10 minutes or more - "Sample Film (2021)" - is a film, not a preview
  clip, and shows up.
- **One rule for disk room** (#29). The same disk could be "OK" on the System page and
  "nearly full" under the library: one counted what's left, the other a share of the disk -
  95% used said "nearly full" with 20 GB free, and with 400 GB on a big disk. The library's
  check now counts room for about two of its biggest videos too, and says when it's the
  disk with Hemma's data.

## 0.7.0 (2026-10-14)

Safer with your data.

### New

- **A damaged database is rescued** (#37). On a memory card that's usually after a power cut.
  Hemma used to start with whatever it happened to read up to the damage - half the notes,
  say - and say nothing. Now it keeps the damaged file aside untouched, copies every row it
  can still read into a fresh one (skipping only the damaged parts, not everything after
  them) and carries on. The System page and a notice on admins' pages say what happened,
  which kinds of things lost rows and where the damaged file is, until an admin presses
  **Got it**.
- **The database has a format number** (#38). Changes to how data is kept run once, after a
  copy of the database is put aside (`hemma.db.before-format-<n>`). Going back to an older
  Hemma works as long as the newer one only added things; when it changed something the older
  one would misread, the older one refuses to start and says why, instead of running on it.
- **The installer keeps the version it replaces** as `/opt/hemma/hemma.prev`: the way back
  is one command (docs/raspberry-pi.md).
- **The time zone is on the System page** (#39): what the clock says and which zone it's in.
  A Pi still on UTC - its own setting - gets a note on how to set yours before people add
  things, and a change of zone says that bookings and events from before may look shifted.

### Fixes

- Saving that fails because the database is damaged says so, and that a restart rescues it,
  instead of "is the disk full?".
- The library cache is dropped and filled again when its layout changes, instead of breaking
  quietly (#38).

## 0.6.1 (2026-10-14)

Hardening, from an outside review of 0.6.0.

### Security

- **One device can't lock everyone out any more** (#32): a device that asked for something and
  then read the answer a few bytes at a time kept its connection for good, so a dozen addresses
  could take every slot. When Hemma is full, such slow readers now make room for newcomers.
- **Only a TV picks who's watching** (#33): any display account, the hallway tablet too, could
  pick anyone under "Who's watching?" and then see, change or clear their Continue watching.
  Now only a display account that shows a TV screen (Screens, Edit, Shows on) can, and one that
  stops showing it forgets who was watching.
- **No more passwords in the log** (#34): a password typed into the name box (a phone filling
  the wrong one) went into the log every admin reads. A failed sign-in now names the account
  only when the name is one, and otherwise just says how long it was.
- **Video keys expire and stay on their screen** (#78): the key on a video's address (0.6.0)
  worked from any device for as long as the sign-in lasted. Now it works only from the address
  of the screen it was made for, for about two days.
- **A browser can't flood the log** (#79): the limit of 20 trouble lines from a browser started
  again with every reload. It's now 20 an hour per person; a line repeating the last one is only
  counted, and what was left out is said once. A browser's user agent goes with its first line
  only.

## 0.6.0 (2026-10-13)

The wall.

### New

- **A new wall**: the wall tablet is a Screen now, put together like the TV's. At the top a
  big clock and the date, what's on **now and next** (a booking that's running, the next
  thing in the calendar) and **who's home**. Below, cards as tall as what they show, in
  columns: Today, Bookings (each thing by name - free, or whose and until when), **Coming
  days** (the week ahead; a visit over several days shows once, "until Sun"), the shopping
  list, notes, what's on Live TV, what's new to watch, and the latest things people did (not
  the network's own news). A card with nothing to show stays away. Admins pick the cards and
  their order under Screens → Edit, and can make more walls (New screen → Wall). Every
  household gets a wall to start from; `/wall` opens it.
- Calendar events now reach the shared agenda too (the wall's "now and next").

### Changes

- The old wall page is gone: `/wall` and its buttons lead to the new one.
- **A browser's own trouble goes in the log** (System page): a script error, a video that
  errs, or one that still hasn't started 25 seconds after it should have - with what the
  browser says about it. A TV's browser has no console anyone can read.

### Fixes

- **Videos and Live TV play on Samsung TVs again.** After an update, the TV's browser lets its
  video player fetch the video without the browser's sign-in, so Hemma sent it the sign-in page
  and every video and channel "buffered" or failed. Video and subtitle addresses now carry their
  own key: it works for that one video, and only while the sign-in it was made for lasts (0.6.1
  ties it to the screen's address and lets it expire).
- The player on TVs: a video the TV's browser won't start by itself shows the big play
  button again instead of what looked like endless loading, and OK plus the remote's
  play/pause/rewind/forward buttons work where the TV names its keys differently. A Live TV
  channel that won't start by itself gets the play button too.
- **Two players**: a video Hemma's player can't play gets the browser's own player in its
  place, from the same spot (Live TV too, still without pausing), with the "Next episode" card
  and full screen as before; the next video tries Hemma's player again. A video neither can
  play says why instead of loading forever.

## 0.5.0 (2026-10-12)

The player.

### New

- **Hemma's own video player**: the same controls in every browser and on the TV - play,
  10 s back and forward, a seek bar, time, subtitles, sound and full screen. They fade
  while you watch and come back with a move or a key. Keys: Space or K, Left/Right,
  M, F; on the TV, OK plays and pauses, Left/Right wind, Down goes to the buttons. A
  click on the picture pauses, a tap shows the buttons, a double-click goes full screen.
  Live TV gets the same bar with just Live, the sound and full screen.
- **Binge-friendly series**: full screen stays full screen when the next episode starts. In
  the last 30 seconds a "Next episode in 12 s" card offers Play now (and Hide); otherwise
  the next one just starts. On the TV the card takes the focus, so OK plays it now. A
  subtitle language you pick stays on for the next episodes.
- **Full screen for Live TV**: changing channel, or the next programme starting, stays in
  full screen. If the TV won't start a channel with sound, the player says "Sound off".

### Fixes

- Leaving a Live TV channel no longer leaves its sound playing in the background (each
  visit added one more voice).

## 0.4.0 (2026-10-12)

The look, round one.

### New

- **Watch and TV are one section**: Watch has the tabs Home, Movies, Series, **Live TV** and
  Folders, and its home shows what's on now. New episodes of a series show as one card
  ("70's Show · 8 new episodes"), and each series gets its own poster colour.
- **Screens: Hemma on the TV.** A full-screen, streaming-app view with big rows you move
  through with the remote's arrow keys; an admin picks the rows and their order (Screens →
  Edit) and which TV shows it. The TV asks *Who's watching?*: pick yourself, no password,
  and your progress follows you (it asks again after 4 hours without use). Videos and
  channels open full screen with a Back button.
- **Live TV is live**: a channel can't be paused, wound or sped up any more - it always
  plays what's on now. The only thing to change is the sound (a Sound on / Sound off
  button; OK on the remote). If the TV won't start it with sound, it starts muted.

### Fixes

- Forms with typed-in text no longer lose it when another screen's change arrives.

## 0.3.0 (2026-10-12)

### Security

- **The video converter** no longer reads past the end of a picture when an MPEG-2 video
  changes its picture size part-way (#4): such files are refused. It also refuses files
  that would make far more pictures or sound blocks than their length allows, instead of
  running out of memory.
- **Signing in** can't freeze Hemma any more: checking passwords from devices that haven't
  signed in before may use at most a quarter of the server's time, so a flood of guesses
  slows only new sign-ins (#5).
- **Signed-out screens** get a small share of the server's connections, so one device
  can't use up every slot (#5).
- **Admins sign in again every 30 days**: Hemma runs on plain http, so a stolen admin
  cookie stops working within a month. Members keep a year (#5).
- **The setup code's** wrong-guess limit keeps counting when its table is full (#5).
- **The network list** has a limit (1,024 devices): made-up devices can't grow it, the page
  or the activity feed without end. Device names from the network now need an answer to
  the exact question Hemma asked (#10).
- **/api** shows members only their own preferences and watch history; admins see all (#5).
- **Rescanning the library** is for admins, and members an admin allows (#5).
- **A quick "live" message** can only run previews, nothing that changes data (#8).
- **CI** runs the whole test suite under AddressSanitizer and UBSan, and fuzzes the video
  converter, on every push (#6). The workflow's token is read-only (#15).

### New

- **System** (admins, under More on a phone): checks in plain words that everything works
  (saving, disk space, the password secret, library folders, the network page...), a
  getting-started list that ticks itself off (also as a card on the admin's home page),
  Hemma's own log live with search and a download, and what to do when something's wrong.
  Hemma keeps its log in the data folder (`hemma.log`, about 1 MB, plus one older file),
  so the lines before a restart or a crash are still there.
- The Pi installer starts Hemma, checks that it answers, and ends with the address, the
  setup code and the next steps - or with the last log lines and what to check. Hemma's
  own startup says where to open it too. `GET /healthz` answers `ok` for scripts.
- `hemma --version` also names the commit it was built from.

### Fixes

- **Sign in with your name** as well as your username ("Adam" for the admin "admin"), and
  a "Not Eva? Sign in as someone else" link under the sign-in page's "Welcome back" (#3).
- Hemma says so in its log when the password secret (`HEMMA_AUTH_SALT`) isn't the one the
  passwords were made with, and when one account gets locked from many devices (#3).
- You can't leave the household without an admin who can sign in any more (#14).
- **Settings file:** a UTF-8 BOM before `HEMMA_AUTH_SALT=` no longer locks everyone out; a
  line that isn't `KEY=VALUE` no longer stops the rest from loading; `KEY= # note` is
  empty; a settings file that is a link stays one (#12).
- **Uploads and conversions** never write the same file (#11), and conversions keep 1 GB
  free on the drive, like uploads (#9).
- **Quick-add dates:** "31 sep" and ranges that end before they start are refused with a
  reason; "tonight 8" is 20:00; bookings around the clock changes in spring and autumn come
  out right (#13).
- Display accounts can't be picked as the person for a booking, to-do or event (#14).
- **On a Pi** Hemma waits (up to 90 s) at boot for the clock to be set, so "today" isn't
  wrong after a power cut (#15).
- **Windows:** the guide runs Hemma as the limited *Local Service* account instead of
  SYSTEM, with access to `C:\Hemma` and the movie folders only, and the log no longer
  grows forever (#15).

### Docs

- The README no longer says your Wi-Fi keeps Hemma's traffic private from others on it (#7).
- Running Hemma on a laptop that leaves the house (#15).
