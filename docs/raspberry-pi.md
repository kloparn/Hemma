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

There are two ways. Pick one.

### A. Build it on a PC and copy one file (fastest)

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
option B is the simplest.

### B. Build it on the Pi itself

On the Pi:

```sh
sudo apt install -y build-essential git
git clone <project URL> hemma      # or download the ZIP from the project page and unzip it
cd hemma
make
```

> **TODO (owner):** put the project's public address here once it's published.

The first build takes a few minutes, because the database engine (SQLite) is compiled
once. On a Pi with only 512 MB of memory (Zero, Zero 2) the build can run out of memory.
If it stops with `Killed`, use option A instead. The result is `build/hemma`. If you also
install [zig](https://ziglang.org), `make` includes the in-browser video converter;
without it, uploads simply send the original files.

## 4. Install it as a service

The installer sets Hemma up to start whenever the Pi starts, as a locked-down background
service. On the Pi, in the folder with the files from step 3:

```sh
sudo sh install-pi.sh ./hemma-pi64                 # option A
sudo sh deploy/install-pi.sh build/hemma           # option B (from the hemma folder)
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

**Update Hemma.** Copy the new `hemma-pi64` over (step 3A), then run the installer
again. It keeps your settings, database and media folders, and restarts the service:

```sh
sudo sh install-pi.sh ./hemma-pi64
```

If you built on the Pi (option B in step 3): `cd hemma && git pull && make && sudo make install`.

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

Everything Hemma knows (people, bookings, plans, notes, who watched what) is in one file:
`/var/lib/hemma/hemma.db`. To copy it safely while Hemma runs, use SQLite's own backup
command:

```sh
sudo apt install -y sqlite3
sudo sqlite3 /var/lib/hemma/hemma.db ".backup '/home/you/hemma-backup.db'"
sudo chown you: /home/you/hemma-backup.db
```

Or stop Hemma for a moment and copy the file. When Hemma stops, it folds everything into
`hemma.db`:

```sh
sudo systemctl stop hemma
sudo cp /var/lib/hemma/hemma.db /home/you/hemma-backup.db
sudo systemctl start hemma
sudo chown you: /home/you/hemma-backup.db
```

Then copy it to another computer. Run this **on that computer**, not on the Pi:

```sh
scp you@hemma.local:hemma-backup.db .
```

**The secret salt.** Passwords in the database only work together with the
`HEMMA_AUTH_SALT` line in `/etc/hemma/hemma.env`. Keep a copy of that line somewhere
safe and *separate* from the database backups, for example in a password manager.
Without it, a restored database can't sign anyone in. Kept apart, a stolen backup is
much harder to attack.

**Restoring:** stop Hemma, put the backup in place, and remove the old WAL files so
they aren't applied to the restored database:

```sh
sudo systemctl stop hemma
sudo cp /home/you/hemma-backup.db /var/lib/hemma/hemma.db
sudo rm -f /var/lib/hemma/hemma.db-wal /var/lib/hemma/hemma.db-shm
sudo chown hemma:hemma /var/lib/hemma/hemma.db
sudo systemctl start hemma
```

Movies aren't in the database; back those up the way you back up any big files.

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
