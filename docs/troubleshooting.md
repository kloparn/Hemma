# Is it working? Finding out what's wrong

Hemma says how it's doing in three places: its **System** page, its **log**, and a tiny
address for scripts. This page shows where to look, explains the usual problems, and
says what to send when you ask someone for help.

The examples use `192.168.1.50` for the server and `hemma.local` for its name. Yours
will be different.

## Right after installing

- **Raspberry Pi or Linux (the installer).** The installer starts Hemma, waits until it
  answers, and then prints `Hemma is running.`, the addresses to open, the setup code (the
  first time) and the next steps. If Hemma didn't come up, it says so, prints Hemma's last
  log lines and lists the usual causes.
- **Started by hand (Windows, a Mac, Linux).** Right after it starts, Hemma prints a block
  like this in its window:

  ```
  ------------------------------------------------------------
  Hemma 0.x.y (1a2b3c4d5e6f) is running.
    Open it on a phone or computer at home:  http://192.168.1.50:8080
                                        or:  http://hemma.local:8080
    First time? Open that address and type the setup code shown above.
    The log: this window, data/hemma.log and Hemma's System page
    Stop it: Ctrl+C
  ------------------------------------------------------------
  ```

Then open the address on a phone, create the admin account (see
[accounts.md](accounts.md)) and go to **System**.

## The System page

**System** is in the menu for admins (on a phone it's under **More**). Only admins see
it: the log names devices and people at home.

- **Checks** says in plain words what works and what needs a look: the version and how
  long Hemma has been running, the address other devices open, whether changes are being
  saved, the room left on the disk, whether passwords can be checked, problems in the
  log, the library folders and their disks, the video converter, the home network and
  device discovery. Anything that isn't right comes first, with one line on what to do.
  When something needs a look, admins also see a small card about it on the home page.
- **Getting started** (on the same tab) ticks itself off as you go: name your home, add
  the household, add your films and series, open Hemma on a phone, save your recovery
  codes, and keep a copy of the password secret. **Hide this** puts it away.
- **Log** shows what Hemma is doing, live, newest first. Pick **Warnings & problems** to
  see only those, or search. **Download the log** saves it as a file.
  **Detailed logging until Hemma restarts** adds more lines while you chase a problem.
- **Help** lists the commands for this very machine and where its files are.

If Hemma stopped without warning the last time (a power cut, a crash), the Checks say so,
and the log still has the lines from before, behind a "Hemma restarted" line.

## Where the log is

Hemma writes every line to two places:

- **Its own file**, `hemma.log` in its data folder: `/var/lib/hemma/hemma.log` on a Pi
  installed with the installer, `data/hemma.log` (or the folder you gave `--data`)
  elsewhere. At about 1 MB it becomes `hemma.log.1` and a new one starts, so it never
  grows past ~2 MB. The System page reads it back after a restart.
- **Where it was started**: on a Pi or Linux service, the system journal; started by hand,
  the window it runs in.

On a Pi or another Linux server installed with the installer:

```sh
systemctl status hemma                   # running or not, and its last lines
journalctl -u hemma -f                   # follow the log live (Ctrl+C stops watching)
journalctl -u hemma -n 100 --no-pager    # the last 100 lines
sudo systemctl restart hemma             # restart (after changing the settings file)
```

On a Mac started at login ([macos.md](macos.md)): `tail -f ~/Hemma/hemma.log`. On Windows
in the background ([windows.md](windows.md)):
`Get-Content C:\Hemma\data\hemma.log -Tail 20 -Wait` in PowerShell.

Each line starts with the time and a letter: `I` information, `W` a warning (worth a look),
`E` a problem, `D` a detail (only with detailed logging).

## For scripts: /healthz

`http://192.168.1.50/healthz` answers `ok` (and nothing else) while Hemma serves pages. It
needs no sign-in and tells nothing about the household. The installer uses it to check
that Hemma came up; a monitoring script can too:

```sh
curl -fsS http://192.168.1.50/healthz
```

## The usual problems

**A phone or the TV can't open Hemma.**
Use the address the Checks show (or the installer printed), and type the `http://` part:
Hemma doesn't do https, and a browser that tries https gets no answer. The phone must be
on the home Wi-Fi, not mobile data. A Windows PC running Hemma must let it through its
firewall ([windows.md](windows.md#the-firewall-question)). If the address changed, give
the server a fixed one in your router ([home-network.md](home-network.md)). Some TVs don't
understand `hemma.local`: give them the number. When the System page's **Address** check
says only this computer can open Hemma, it was started with `--host 127.0.0.1` (or
`HEMMA_HOST=127.0.0.1` in the settings file): leave that out, or use `0.0.0.0`, and restart it.

**Hemma doesn't start.**
Its last log lines say why. The usual reasons: another program already uses the port
(`sudo ss -ltnp | grep ':80 '` shows which), a mistake in the settings file
(`/etc/hemma/hemma.env` on a Pi), or the data folder can't be written. When it's the
data itself - a newer Hemma's, or damaged past rescuing (both below) - the reason is in
`hemma.log` too, and a Pi's service doesn't start it again and again (exit code 78):
after fixing it, `sudo systemctl restart hemma`.

**Nobody can sign in.**
If the Checks (or the log at start) say the password secret isn't the one the passwords
were made with, the `HEMMA_AUTH_SALT` line in the settings file changed: put the original
back and restart. One person forgot their password: a recovery code, or
`hemma --reset-password NAME` on the server. All of it is in [accounts.md](accounts.md).

**Changes don't stick / "Saving to disk failed".**
The disk with Hemma's data is full (or broke). Make room and restart Hemma: changes made
since the problem started were only in memory.

**Uploads stop, or say there's no room.**
The Checks list each library folder: whether Hemma may write into it, and the room left
on its disk. On a Pi, run the installer again with every media folder after the program
file, so Hemma may write there ([raspberry-pi.md](raspberry-pi.md#7-a-usb-disk-for-movies)).

**A video won't play on some devices.**
DVD rips, MKV files and Dolby sound don't play everywhere. **Watch → Library settings**
makes a copy of each that does.

**A video plays, but without sound.**
Its sound is most likely Dolby (Dolby Digital or Digital Plus - "DD5.1", "DDP5.1" in a file
name): Chrome and Firefox play the picture and stay silent, Safari and Edge play it. The
video's page says so under the player. Press **Fix the sound** there (or **Convert** on
**Watch → Library settings** for every such video at once): Hemma makes a copy next to it with
AAC sound, which every browser plays - the picture is copied as it is, so it's quick. Uploads
from a browser make that copy before sending (the box *Make videos play everywhere on this
device first*). If an upload couldn't, its card says which videos went up unconverted, and why.

**Subtitles inside an MKV don't show.**
Browsers can't read subtitles inside a video file, only separate ones. Hemma takes them out and
saves them next to the video - text ones as `.srt` ("Movie.en.srt"), DVD and Blu-ray ones (which
are pictures) as `.idx`/`.sub` and `.sup`, which Hemma's player draws. It does that when it converts
an MKV (on the server or while uploading), and by itself for the MKVs already in the library, a few
at a time while nobody is watching (**Watch → Library settings → Subtitles inside videos** shows how
far it's got). A video whose MKV isn't on the server - only its converted copy is - gets them when
you upload the MKV again: the upload sees the library has the video and sends only its subtitles.
An `.srt` you put next to the video yourself works too, and one you upload on its own finds its
episode or film by its name ("Show S01E02.en.srt").

**"Database rescued" on the System page.**
At the start, Hemma found its database damaged and rescued what it could still read; the
check says how much of each kind of thing was kept. See [A damaged database](#a-damaged-database)
below. Press **Got it** once you've read it.

**"Hemma's data ... is damaged, and a rescue got nothing back" in the log, and it won't start.**
The rescue got next to nothing back, so Hemma left everything as it was rather than start
as a new household. See [A damaged database](#a-damaged-database).

**"This data was last used by Hemma 1.2 ..." in the log, and it won't start.**
You went back to an older version, and the newer one had changed the data in a way the older
one would misread. Either go forward again - swap the programs back
([raspberry-pi.md](raspberry-pi.md#going-back), the same commands as going back) - or keep
the older one and put back the copy of the database the message names
(`hemma.db.before-format-<n>`, made just before the change; the installer's
`hemma.db.before-install-<when>` works too). What was added since that copy is lost.

**Times look shifted after the time zone was changed.**
Hemma keeps a time as a moment, worked out from the clock's zone when it was typed, so a Pi
left on UTC for a week and then set to your zone shows that week's bookings hours off. At the
next start Hemma notices: things that last whole days (all-day events, chores' days, whole-day
bookings) keep their days by themselves, and the System page lists the times typed before,
as they'd be if moved back ("Dentist · Tue 16:30 → 14:30"). If the old zone was wrong, press
**Move them back to the times typed**; if Hemma really moved to another zone, press **Got it**.

**A phone's calendar doesn't update.**
It only can on the home Wi-Fi (Hemma isn't on the internet), and phones choose when to look
(an iPhone: Settings → Calendar → Accounts → Fetch New Data). Was a new link made, or the link
turned off? Then add the new one under **Household → Your calendar on your phone**.

**A library folder is missing**, or **the movie disk is missing**.
The disk isn't plugged in or mounted. A disk Hemma set up (**Use it for movies**) is mounted
again by itself once it's plugged back in. One set up by hand: plug it in (on a Pi, check
`/etc/fstab` so it's mounted at every start), then rescan in **Watch → Library settings**.

**A new disk doesn't show up under Library settings → Disks.**
Hemma offers USB disks only, not one it can see mounted already (then add its folder as a
library folder), and not the memory card. `sudo hemma-disks` lists what the Pi sees. A disk
with an Apple (APFS/HFS+) or encrypted file system can't be used; one with nothing on it is
offered for formatting.

## A damaged database

Everything Hemma knows is in one file, `hemma.db` in its data folder (`/var/lib/hemma` on a
Pi). A power cut while it's being written, or a memory card wearing out, can damage it. Hemma
checks it at every start. When it's damaged:

- It first copies it aside, untouched: `hemma.db.damaged-<date>-<time>` (the newest three
  copies stay).
- It reads every row it still can into a fresh `hemma.db`, with SQLite's own recovery (what
  `sqlite3 ... .recover` does), and carries on. The System page says **Database rescued**,
  with how many of each kind of thing were kept: Hemma notes how many there are in
  `hemma.db.counts` while it runs, to compare with.
- Rows it found but couldn't tell where they belong go into a table called
  `lost_and_found` in the new file. Nothing is thrown away.
- Until an admin presses **Got it**, the first-run welcome page stays closed. If no admin
  account came back, everyone sees a page saying so; bring one back as in **Locked out** in
  [accounts.md](accounts.md).
- If the rescue gets next to nothing back - no rows at all, none of the household's accounts,
  or under a tenth of what was there - Hemma doesn't start: starting as a new household would
  invite you to set it up again over your data. It changes nothing and says why in the log
  and in `hemma.log`.

A memory card that damaged one file may damage more: think about a new one.

**Getting more back.** A backup from before is best: **System → Backups → Go back to this**
on the last good night (or `hemma --restore`, see [raspberry-pi.md](raspberry-pi.md#backups)).
That brings the lost rows back, and loses what was added since.

Without a backup, SQLite's own tool sometimes gets more back. Try it on a copy (on a Pi;
use your own copy's name):

```sh
sudo apt install -y sqlite3
sudo systemctl stop hemma
cd /var/lib/hemma
sudo cp hemma.db.damaged-2026-10-17-0930 /tmp/damaged.db
# and its -wal file, if there is one: sudo cp hemma.db.damaged-2026-10-17-0930-wal /tmp/damaged.db-wal
sudo sqlite3 /tmp/damaged.db .recover | sudo sqlite3 /tmp/recovered.db
sudo sqlite3 /tmp/recovered.db "SELECT count(*) FROM person"
```

If that got more back than Hemma did, use it:

```sh
sudo mv hemma.db hemma.db.rescued
sudo install -o hemma -g hemma -m 600 /tmp/recovered.db hemma.db
sudo systemctl start hemma
```

**Passwords** are made with the secret in the settings file (`HEMMA_AUTH_SALT`): a
database only lets people sign in with the secret it was made with. Keep the settings file
as it is.

**To really start over**, move `hemma.db` away (don't delete it - you may want it back) and
start Hemma: the welcome page creates a new admin.

## Asking for help

Send:

1. the version - on the System page, or `hemma --version`;
2. what you did, what you expected, and what happened instead;
3. the log: **System → Log → Download the log** (or `hemma.log` from the data folder).

The log names devices, addresses and people at home: share it only with someone you
trust, and read it through first if you like.
