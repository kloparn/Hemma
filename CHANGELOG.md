# Changelog

What changed in each release of Hemma. `hemma --version` says which one you run (and the
commit it was built from).

## 0.3.0 (not released yet)

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
