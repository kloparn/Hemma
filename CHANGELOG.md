# Changelog

What changed in each release of Hemma. `hemma --version` says which one you run (and the
commit it was built from).

## 0.6.1 (not released yet)

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
