# Hemma on a Raspberry Pi

A Raspberry Pi is the recommended home for Hemma: small, quiet, cheap to run, and plenty
fast. This guide goes from an empty memory card to Hemma starting by itself every time
the Pi is switched on.

You'll need:

- a Raspberry Pi with its power supply (use the official one: a weak supply causes odd
  problems),
- a microSD card (8 GB or more is plenty: movies go on a separate disk),
- another computer (Windows, Mac or Linux) to set things up from,
- a network cable to the router (recommended) or your Wi-Fi name and password.

The Pi has no screen or keyboard in this setup. You'll talk to it from your computer over
**SSH**. If that's new to you, read [SSH: talking to a computer without a screen](ssh.md)
first. It's short.

## 1. Choose a Pi

| Model | Good for | Which Hemma file |
|---|---|---|
| **Pi 5** or **Pi 4** | Everything, including several movies at once. Gigabit network and fast USB 3 for the movie disk | `hemma-pi64` |
| **Pi 3B+** / **Pi 3B** | The whole household and a few movies at once. The 3B's network port is 100 Mbit (about ten 1080p streams); the 3B+ is faster but shares USB 2 with the disk | `hemma-pi64` |
| **Pi Zero 2 W** | Planning, bookings and the board; the odd movie. Wi-Fi only, 512 MB of memory | `hemma-pi64` |
| Pi 1, Pi Zero, Pi Zero W | The planning side. Slow, and 32-bit only | `hemma-pi32` |

Hemma itself needs very little: on a Pi 3B+ it uses a few megabytes of memory. For movies,
the network is the limit, not the Pi. Use a cable if you can.

**32 or 64-bit?** Use **Raspberry Pi OS (64-bit)** on a Pi 3, 4, 5 or Zero 2 W. That
matches `hemma-pi64`. The older models only run the 32-bit system, which matches
`hemma-pi32`. If you're unsure later, run `uname -m` on the Pi: `aarch64` means use
`hemma-pi64`; `armv7l` or `armv6l` means use `hemma-pi32`.

## 2. Put Raspberry Pi OS on the card

1. On your computer, install **Raspberry Pi Imager** from
   <https://www.raspberrypi.com/software/> and start it.
2. **Device:** pick your Pi model.
3. **Operating system:** *Raspberry Pi OS (other)* → **Raspberry Pi OS Lite (64-bit)**.
   "Lite" means no desktop, which a server doesn't need. Pick the 32-bit Lite version
   for a Pi 1 / Zero / Zero W.
4. **Storage:** your microSD card. Everything on it will be erased.
5. **Customisation.** Imager asks whether to customise the settings (on older versions:
   *Edit settings*). Say yes and fill in:
   - **Hostname:** `hemma`. The Pi will then answer to `hemma.local`.
   - **Username and password:** your own. You'll use them for SSH and `sudo`.
   - **Wi-Fi:** name, password and country. You can skip this if you use a cable.
   - **Time zone and keyboard:** your own. The time zone matters: bookings and the
     calendar use the Pi's clock.
   - **SSH: enable it.** Choose *public-key authentication* if you already have an SSH
     key (see [ssh.md](ssh.md#keys-better-than-passwords)), otherwise a password is fine
     for now.
6. Write the card, put it in the Pi, connect the cable (if any) and the power. The first
   start takes a couple of minutes.

Now connect from your computer (replace `you` with the username you chose):

```sh
ssh you@hemma.local
```

If `hemma.local` isn't found, use the Pi's IP address instead. Your router's list of
devices shows it; see [home-network.md](home-network.md#find-the-servers-address).
Bring the system up to date once:

```sh
sudo apt update && sudo apt full-upgrade -y
```

## 3. Get Hemma onto the Pi

There are three ways. Pick one.

### A. Download it (simplest)

On the Pi, fetch the latest release and unpack it:

```sh
wget https://github.com/kloparn/Hemma/releases/latest/download/hemma-pi64.tar.gz
tar xzf hemma-pi64.tar.gz && cd hemma-pi64
```

For a 32-bit Raspberry Pi OS (and on a Pi 1, Zero or Zero W) use `hemma-pi32.tar.gz` instead.
The folder holds the program, the installer and the service file it needs, a `README.txt`
and `SHA256SUMS` (`sha256sum -c SHA256SUMS` checks the files). These downloads include
the in-browser video converter. Go on with step 4.

### B. Build it on a PC and copy one file

On a **Windows** PC with the Hemma source code and [zig](https://ziglang.org) installed
(see [windows.md](windows.md#1-install-zig)):

```powershell
.\build.ps1 pi      # 64-bit -> build\hemma-pi64
.\build.ps1 pi32    # 32-bit -> build\hemma-pi32 (Pi 1 / Zero / Zero W)
```

The result is a single file with no dependencies. It works on any Raspberry Pi OS
version. Copy it to the Pi together with the two install files:

```sh
scp build/hemma-pi64 deploy/hemma.service deploy/install-pi.sh you@hemma.local:
```

(The `:` at the end means "into my home folder on the Pi".) From a Mac or Linux PC,
option A or C is the simplest.

### C. Build it on the Pi itself

On the Pi:

```sh
sudo apt install -y build-essential git
git clone https://github.com/kloparn/Hemma.git hemma      # or Code -> Download ZIP on GitHub, and unzip it
cd hemma
make
```

The first build takes a few minutes, because the database engine (SQLite) is compiled
once. On a Pi with only 512 MB of memory (Zero, Zero 2) the build can run out of memory.
If it stops with `Killed`, use option A or B instead. The result is `build/hemma`. If you also
install [zig](https://ziglang.org), `make` includes the in-browser video converter;
without it, uploads simply send the original files.

## 4. Install it as a service

The installer sets Hemma up to start whenever the Pi starts, as a locked-down background
service. On the Pi, in the folder with the files from step 3:

```sh
sudo sh install-pi.sh ./hemma-pi64                 # options A and B
sudo sh deploy/install-pi.sh build/hemma           # option C (from the hemma folder)
```

Add the folders that hold your movies after the program file if you already have them
(for example `sudo sh install-pi.sh ./hemma-pi64 /srv/media`; see
[step 7](#7-a-usb-disk-for-movies)). Hemma may then save uploads there.

What the installer does:

- creates a system user `hemma` that the service runs as (not root),
- copies the program to `/opt/hemma/hemma`,
- creates `/var/lib/hemma` for the database,
- creates `/etc/hemma/hemma.env`, the settings file, with a fresh secret
  `HEMMA_AUTH_SALT`, a one-time `HEMMA_SETUP_CODE` and `HEMMA_PORT=80` so the address
  needs no port number,
- installs the service `hemma` (`/etc/systemd/system/hemma.service`).

Then it starts Hemma (and makes it start on every boot), waits until it answers, and
prints `Hemma is running.`, the addresses to open, the next steps and the handy commands.
The first time that includes your **setup code** (for example `7KQ4-M2XP`): write it down
or keep the window open. If Hemma didn't come up, the installer says so and shows its
last log lines - see [troubleshooting.md](troubleshooting.md).

Now open **`http://hemma.local`** in a browser on any device at home, type the **setup
code** and **create the admin account**. That's you: you'll add everyone else under
**Household**. Lost the code? `sudo grep SETUP /etc/hemma/hemma.env` shows it. It works
once: after that the welcome page and the code are gone. More about
accounts, recovery codes and forgotten passwords: [accounts.md](accounts.md).

> **Prefer to set the admin up from the settings file?** (Handy on a server you set up
> without a browser at hand.) Before the first start, put `HEMMA_ADMIN_USER` and
> `HEMMA_ADMIN_PASSWORD` in `/etc/hemma/hemma.env` (`sudo nano /etc/hemma/hemma.env`).
> After the first sign-in, remove the password line again. Details in
> [accounts.md](accounts.md).

## 5. Where everything lives

| What | Where |
|---|---|
| The program | `/opt/hemma/hemma` |
| Settings and the secret salt | `/etc/hemma/hemma.env` (only root can change it; the service can read it) |
| The database | `/var/lib/hemma/hemma.db` (plus `hemma.db-wal` and `hemma.db-shm` while it runs) |
| The service | `/etc/systemd/system/hemma.service` |
| Folders Hemma may write to (uploads) | `/etc/systemd/system/hemma.service.d/media.conf` |
| Your movies and series | wherever you mount the disk, for example `/srv/media` |

To change a setting: `sudo nano /etc/hemma/hemma.env`, then `sudo systemctl restart hemma`.
The settings are explained in [.env.example](../.env.example).

## 6. Everyday care

**Is it running? What is it doing?**

In Hemma itself, admins open **System** (under **More** on a phone): it checks that
everything works, in plain words, and shows the log live. On the Pi:

```sh
systemctl status hemma          # running or not, plus the last few log lines
journalctl -u hemma -f          # follow the log live (Ctrl+C to stop watching)
journalctl -u hemma | grep "is up on"   # the address Hemma announced when it started
```

Hemma also keeps its own log in `/var/lib/hemma/hemma.log`. More in
[troubleshooting.md](troubleshooting.md).

**Restart it:** `sudo systemctl restart hemma`

**Update Hemma.** Download the new release the way you did in step 3A and run its installer.
It keeps your settings, database and media folders, keeps the program it replaces for
[going back](#going-back), and restarts the service:

```sh
wget https://github.com/kloparn/Hemma/releases/latest/download/hemma-pi64.tar.gz
tar xzf hemma-pi64.tar.gz && cd hemma-pi64
sudo sh install-pi.sh ./hemma-pi64
```

(Unpacking over last time's folder is fine.) Built it on a PC (option B)? Copy the new
`hemma-pi64` over and run the installer the same way. Built it on the Pi (option C):
`cd hemma && git pull && make && sudo make install`.

**Go back to the version before**: see [Going back](#going-back), below.

**Update the system** now and then: `sudo apt update && sudo apt full-upgrade -y`, then
`sudo reboot` if it asks for one.

**Health check.** These are useful if things feel slow. They're explained in
[ssh.md](ssh.md#the-commands-youll-actually-need):

```sh
df -h                     # disk space
free -h                   # memory
vcgencmd measure_temp     # temperature
vcgencmd get_throttled    # 0x0 = all good; anything else = power supply or heat trouble
```

### Backups

Everything Hemma knows (people, bookings, plans, notes, who watched what) is in one file,
`/var/lib/hemma/hemma.db`, on the memory card: the part of a Pi most likely to fail. So
Hemma backs it up by itself:

- **Every night** (around 03:00) it copies the database, checks the copy and keeps the
  newest of the last 7 days. The first one is made a few minutes after Hemma starts; a
  computer that was off at night gets one soon after it's on again.
- **Where:** in a hidden `.hemma-backups` folder in your first library folder on another
  disk than the database. With the USB disk from section 7, that's
  `/mnt/media/library/.hemma-backups`: the backups survive a dead memory card. Without
  another disk they go to `/var/lib/hemma/backups`, which protects against damage and
  mistakes, but not against the card dying. **System → Backups** shows where they are,
  and can send them to another folder (one Hemma may write to, such as a media folder you
  gave `install-pi.sh`).
- **System → Checks** says when the last backup was made, and warns when one failed or
  the newest is more than two days old.

**Going back to a backup.** Open **System → Backups** and press **Go back to this** next to
the backup you want. Everything changed since it was made is gone. Hemma checks the
backup, keeps what's there now as `hemma.db.before-restore-<date>`, and restarts with the
backup, which takes a few seconds. Afterwards, **System → Checks** says "Backup put back".

**A copy to keep elsewhere.** **Download** on System → Backups gives one `.tar` file with
two things in it:

- the database;
- `hemma-secret.txt`, the password secret (`HEMMA_AUTH_SALT`) that every password and
  recovery code is made with.

Keep the download like a password: in a password manager, or on an encrypted USB stick.
The nightly backups leave the secret out, so a nightly backup on its own can't sign anyone
in on a new install.

**On a new memory card** (or another computer):

1. Install Hemma as in step 4, with the same media folders. Mount the library disk at the
   same path as before (`/mnt/media`, section 7). Watch history finds its videos surest that
   way; it also finds a video that moved or was renamed, by its content or its name.
2. Copy the download (or a nightly `hemma-….db` from the USB disk) to the Pi, and put it
   back:

   ```sh
   sudo install -o hemma -g hemma -m 600 hemma-2026-10-16-0330-with-secret.tar /var/lib/hemma/
   sudo -u hemma /opt/hemma/hemma --restore /var/lib/hemma/hemma-2026-10-16-0330-with-secret.tar \
       --data /var/lib/hemma --env /etc/hemma/hemma.env
   ```

   It checks the file, and says if the settings file needs the old secret. If it does,
   run `sudo nano /etc/hemma/hemma.env` and replace the `HEMMA_AUTH_SALT` line with the
   one it shows. A nightly backup has no secret in it: put back the line you kept. If it's
   lost, give the admin a new password with `--reset-password`
   ([accounts.md](accounts.md)), then give everyone else new ones.
3. Restart Hemma: `sudo systemctl restart hemma`.

**By hand.** SQLite's own backup command copies the database safely while Hemma runs:

```sh
sudo apt install -y sqlite3
sudo sqlite3 /var/lib/hemma/hemma.db ".backup '/home/you/hemma-backup.db'"
sudo chown you: /home/you/hemma-backup.db
```

`hemma --restore` puts such a copy back too, as in step 2 above.

**Your data in other programs.** When you're signed in,
`http://hemma.local/api` lists Hemma's tables and `http://hemma.local/api/booking` (or
`plan_event`, `board_note`, …) gives one as JSON, with times in Unix seconds. It's handy
for your own scripts. Members see their own preferences and watch history; admins see
everything.

Movies aren't in the database: back those up the way you back up any big files.

### Going back

**Go back to the version before.** The installer keeps the program it replaced as
`/opt/hemma/hemma.prev`. Swap the two, so the newer one is still there to come back to
(the same commands again swap them back):

```sh
cd /opt/hemma && sudo mv hemma hemma.x && sudo mv hemma.prev hemma && sudo mv hemma.x hemma.prev
sudo systemctl restart hemma
```

Your data comes along. Only when the newer version changed it in a way older ones would
misread does the older one refuse to start. The log (`journalctl -u hemma`, and
`/var/lib/hemma/hemma.log`) then says which Hemma last used the data, and the service waits
instead of retrying. Then either swap back to the newer one, or keep the older one and put
back the database as it was before. Hemma copies it before such a change
(`hemma.db.before-format-<n>`), and the installer before every update
(`hemma.db.before-install-<when>`, the newest three stay). What was added since that copy is
lost:

```sh
sudo systemctl stop hemma
cd /var/lib/hemma
ls hemma.db.before-*                       # pick one; the example uses an installer copy
sudo mv hemma.db hemma.db.newer
[ ! -f hemma.db-wal ] || sudo mv hemma.db-wal hemma.db.newer-wal   # (its unsaved part, if any)
sudo cp -p hemma.db.before-install-2026-10-17-101500 hemma.db
sudo systemctl start hemma
```

(If the copy has a `-wal` file next to it, copy that too, as `hemma.db-wal`.)

## 7. A USB disk for movies

1. **Plug the disk in** (on a Pi 4/5, use a blue USB 3 port). A large 2.5" hard disk may
   need more power than a Pi 3 can give; a powered USB hub solves that.
2. **Find it:**

   ```sh
   lsblk -f
   ```

   Look for the disk's partition (often `sda1`), its type (`FSTYPE`) and its `UUID`.
3. **A new or empty disk?** Format it as `ext4`, the Linux format, so uploads and
   permissions just work. **This erases the disk**, so double-check the name:

   ```sh
   sudo mkfs.ext4 -L media /dev/sda1
   ```

   A disk that already has your movies on it (from a PC: NTFS or exFAT) can stay as it is.
   See the note below.
4. **Mount it at a fixed place** every time the Pi starts:

   ```sh
   sudo mkdir -p /srv/media
   sudo nano /etc/fstab
   ```

   Add one line at the end (use your disk's UUID from step 2). `nofail` means the Pi
   still starts if the disk is unplugged:

   ```
   UUID=1234abcd-5678-90ef-1234-567890abcdef  /srv/media  ext4  defaults,nofail  0  2
   ```

   Save (Ctrl+O, Enter, Ctrl+X), then:

   ```sh
   sudo systemctl daemon-reload
   sudo mount -a
   df -h /srv/media          # shows the disk's size if it worked
   ```

5. **Let Hemma use it.** Run the installer again with the folder. This allows uploads
   into it:

   ```sh
   sudo sh install-pi.sh ./hemma-pi64 /srv/media
   ```

   Pass the program file you installed from (not `/opt/hemma/hemma` itself). List *all*
   your media folders each time. Run without folders, it keeps the current list.
6. **Tell Hemma where it is:** in Hemma, open **Watch**, then the **Library settings**
   button. Under **Folders**, enter `/srv/media` and press **Save & scan**.

**NTFS or exFAT disks** (formatted on Windows or a Mac) don't have Linux permissions, so
the owner comes from the mount options. When you run the installer with such a folder, it
prints the exact `/etc/fstab` line to use (with `uid=` and `gid=` for the `hemma` user).
Use the type `lsblk -f` shows (`ntfs3` for NTFS on current systems, or `exfat`).

**A NAS or network share** works too: mount it (for example with `cifs` or `nfs` in
`/etc/fstab`), then continue from step 5.

## Tips

- **Give the Pi a fixed address** in your router so TVs and bookmarks keep working. See
  [home-network.md](home-network.md#give-the-server-a-fixed-address).
- **Rename the Pi** later: `sudo raspi-config` → *System Options* → *Hostname*, then
  reboot. Phones then find it as `newname.local`.
- **Turn it off properly:** `sudo poweroff`, and wait for the green light to stop
  blinking before pulling the plug. Pulling the power while it writes can damage the
  memory card.
- **Something wrong?** Hemma's **System** page says what isn't working; `journalctl -u
  hemma -n 50` shows the last 50 log lines. Hemma's messages are written to be read by
  people. See [troubleshooting.md](troubleshooting.md).
