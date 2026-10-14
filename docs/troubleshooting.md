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
  Hemma 0.2.0 is running.
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
understand `hemma.local`: give them the number.

**Hemma doesn't start.**
Its last log lines say why. The usual reasons: another program already uses the port
(`sudo ss -ltnp | grep ':80 '` shows which), a mistake in the settings file
(`/etc/hemma/hemma.env` on a Pi), or the data folder can't be written.

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

**"Database rescued" on the System page.**
At the start, Hemma found its database damaged - on a memory card that's usually after a
power cut, or a card wearing out. It copied the damaged file aside untouched
(`hemma.db.damaged-<date>` in the data folder) and carried on with every row it could still
read. The check says which kinds of things lost rows. If you have a backup from before, stop
Hemma, put the backup in place as `hemma.db`, and start it again: that brings the lost rows
back (and loses what was added since). Otherwise carry on. Either way, a card that damaged
one file may damage more: think about a new one. Press **Got it** once you've read it.

**"Hemma was changed by a newer Hemma" in the log, and it won't start.**
You went back to an older version, and the newer one had changed the data in a way the older
one would misread. Put the newer version back (`/opt/hemma/hemma.prev` is the one the
installer replaced).

**A library folder is missing.**
The disk isn't plugged in or mounted. Plug it in (on a Pi, check `/etc/fstab` so it's
mounted at every start), then rescan in **Watch → Library settings**.

## Asking for help

Send:

1. the version - on the System page, or `hemma --version`;
2. what you did, what you expected, and what happened instead;
3. the log: **System → Log → Download the log** (or `hemma.log` from the data folder).

The log names devices, addresses and people at home: share it only with someone you
trust, and read it through first if you like.
