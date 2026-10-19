# Changelog

What changed in each release of Hemma. `hemma --version` says which one you run (and the
commit it was built from).

## 0.13.0 (not released yet)

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
