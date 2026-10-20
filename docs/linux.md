# Hemma on a Linux computer

Any Linux computer that stays on can run Hemma: an old laptop, a mini PC, a home server
or a NAS that lets you run your own programs. Both x86-64 (Intel/AMD) and ARM work. On a
Raspberry Pi, follow [raspberry-pi.md](raspberry-pi.md) instead. It's the same thing with
Pi-specific tips.

Commands here are for a terminal on that computer, or over [SSH](ssh.md) if it has no
screen.

## 1. Get the program

Hemma is one file. Pick the way that suits you.

### Download it (the quickest way)

The [latest release](https://github.com/kloparn/Hemma/releases/latest) has static programs with no dependencies; they run on
any distribution:

| Computer (`uname -m` says) | Download |
|---|---|
| Intel/AMD 64-bit (`x86_64`) | `hemma-linux-x64.tar.gz` |
| 64-bit ARM (`aarch64`) | `hemma-pi64.tar.gz` |
| 32-bit ARM (`armv7l`, `armv6l`) | `hemma-pi32.tar.gz` |

The "pi" ones are ordinary ARM Linux programs and work on any ARM board, not just a Pi. On the
server:

```sh
wget https://github.com/kloparn/Hemma/releases/latest/download/hemma-linux-x64.tar.gz
tar xzf hemma-linux-x64.tar.gz && cd hemma-linux-x64
```

The folder holds the program, the installer and the service file it needs. To try it first:
`./hemma-linux --demo` (a made-up household in a throwaway folder; the login shows in its output).

### Cross-compiled on a Windows PC

On a Windows PC with zig (see [windows.md](windows.md#1-install-zig)), `build.ps1` makes
static Linux programs. They have no dependencies and run on any distribution:

| Computer (`uname -m` says) | Command | File |
|---|---|---|
| Intel/AMD 64-bit (`x86_64`) | `.\build.ps1 linux` | `build\hemma-linux` |
| 64-bit ARM (`aarch64`) | `.\build.ps1 pi` | `build\hemma-pi64` |
| 32-bit ARM (`armv7l`, `armv6l`) | `.\build.ps1 pi32` | `build\hemma-pi32` |

The "pi" files are ordinary ARM Linux programs and work on any ARM board, not just a Pi.
Copy the file and the two install files to the server (see [ssh.md](ssh.md#copying-files-scp)):

```sh
scp build/hemma-linux deploy/hemma.service deploy/install-pi.sh you@server.local:
```

### Build it on the server

You need a C compiler, `make` and `git`:

```sh
sudo apt install -y build-essential git          # Debian, Ubuntu, Raspberry Pi OS, Mint
sudo dnf install -y gcc make git                 # Fedora, RHEL, Rocky, Alma
sudo pacman -S --needed base-devel git           # Arch, Manjaro
```

Then:

```sh
git clone https://github.com/kloparn/Hemma.git hemma      # or Code -> Download ZIP on GitHub, and unzip it
cd hemma
make            # -> build/hemma (the first build compiles SQLite: a few minutes)
make test       # optional: runs the test suite
```

If [zig](https://ziglang.org) is installed, `make` also builds the in-browser video
converter. Without it, uploads simply send the original files. `make CC=clang` or
`make CC="zig cc"` use another compiler.

## 2. Install it as a service (systemd)

Most distributions use systemd (Debian, Ubuntu, Fedora, Arch, openSUSE, Raspberry Pi
OS…). `deploy/install-pi.sh` works on all of them, despite its name:

```sh
sudo sh install-pi.sh ./hemma-linux                  # a ready-built file
sudo sh deploy/install-pi.sh build/hemma             # built from source (or: sudo make install)
```

Add your media folders after the program file (for example
`sudo sh install-pi.sh ./hemma-linux /srv/media`). The installer then lets Hemma write
uploads there. List all of them each time; without any, it keeps the current list.

It creates a `hemma` system user, `/opt/hemma/hemma`, the database folder
`/var/lib/hemma`, the settings file `/etc/hemma/hemma.env` (with a fresh secret salt and
`HEMMA_PORT=80`) and a hardened `hemma` service. Then it starts Hemma (and on every boot
from now on), waits until it answers, and prints where to open it and the next steps - or,
if it didn't come up, its last log lines ([troubleshooting.md](troubleshooting.md)).
Running it again later updates the program and restarts the service, keeping your
settings and data. Installed from a download (0.21.0 or newer), later updates are one command,
`sudo hemma-upgrade`, or **Install** under System → Updates in Hemma: only what the project
signed goes in, and it goes back by itself if the new version doesn't start
([raspberry-pi.md](raspberry-pi.md#6-everyday-care), "Update Hemma").

Open `http://<server address>` in a browser and **create the admin account**. The welcome
page asks for the **setup code** the installer showed in its last lines (also in
`/etc/hemma/hemma.env`; started by hand, Hemma prints it when it starts). See
[accounts.md](accounts.md); you can also set the first admin up in the settings file
instead.

Day to day:

```sh
systemctl status hemma              # running or not, and its last lines
journalctl -u hemma -f              # the live log
sudo systemctl restart hemma        # after changing /etc/hemma/hemma.env
sudo nano /etc/hemma/hemma.env      # settings (see .env.example)
```

In Hemma, admins open **System**: checks in plain words and the log, live (Hemma also keeps
it in `/var/lib/hemma/hemma.log`). When something's wrong: [troubleshooting.md](troubleshooting.md).

The database is `/var/lib/hemma/hemma.db`. Hemma backs it up every night by itself
(**System → Backups**), as described in [raspberry-pi.md](raspberry-pi.md#backups).

### Things that differ between machines

**Port 80 already taken?** If the computer already runs a web server (nginx, Apache,
a NAS admin page), Hemma can't use port 80. Check with `sudo ss -ltnp | grep ':80 '`. To
use another port, set `HEMMA_PORT=8080` in `/etc/hemma/hemma.env`, then
`sudo systemctl restart hemma`. The address becomes `http://<server>:8080`.

**A firewall?** Let the port in from your home network only, for example:

```sh
sudo ufw allow from 192.168.1.0/24 to any port 80 proto tcp       # Ubuntu (ufw)
sudo firewall-cmd --permanent --add-service=http && sudo firewall-cmd --reload   # Fedora (firewalld)
```

(Use your own network range. For port 8080 with firewalld:
`--add-port=8080/tcp` instead of `--add-service=http`.)

**SELinux (Fedora, RHEL…).** If `systemctl status hemma` shows `status=203/EXEC`,
SELinux is probably refusing to run a program from `/opt`. Label it as a program:
`sudo chcon -t bin_t /opt/hemma/hemma`, then `sudo systemctl restart hemma`. Repeat
after each update.

**Media in your home folder.** The service runs as the `hemma` user, which can't look
inside other people's home folders on many systems. The easy way is a folder outside
`/home`, such as `/srv/media`. Otherwise give `hemma` access to that one folder:

```sh
sudo apt install -y acl        # setfacl, if it's missing
sudo setfacl -m u:hemma:x /home/you
sudo setfacl -R -m u:hemma:rX /home/you/Videos
```

**A USB disk** is mounted the same way as on a Pi; see
[raspberry-pi.md](raspberry-pi.md#7-a-usb-disk-for-movies).

**`.local` names** need an mDNS service. Most desktops have one; on a server, install it:
`sudo apt install -y avahi-daemon` (Debian/Ubuntu) or `sudo dnf install -y avahi`
followed by `sudo systemctl enable --now avahi-daemon` (Fedora). See
[home-network.md](home-network.md#names-hemmalocal-or-the-ip-address).

**A laptop as a server** should never go to sleep. These commands turn sleep off for
good (undo with `unmask`):

```sh
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

To keep it running with the lid closed, set `HandleLidSwitch=ignore` in
`/etc/systemd/logind.conf` and reboot.

**A laptop that leaves the house** would serve Hemma on every network it joins: at a
café, the office or a hotel, everyone on that Wi-Fi could open your sign-in page. A
firewall rule by address range doesn't help much there (other networks use
`192.168.1.x` too). Either don't start it at boot (`sudo systemctl disable hemma`, then
`sudo systemctl start hemma` when you're home), or, with firewalld, open the port only
in the zone of your home connection:

```sh
sudo firewall-cmd --permanent --zone=home --add-service=http && sudo firewall-cmd --reload
nmcli connection modify "Your home Wi-Fi" connection.zone home     # its name: nmcli connection show
```

## 3. Running without systemd

Hemma doesn't need systemd, root or an installer. It's one program you can start
yourself, for example in a folder in your home directory:

```sh
mkdir -p ~/hemma && cd ~/hemma
cp /path/to/hemma-linux ./hemma && chmod +x hemma
./hemma --data ~/hemma/data --env ~/hemma/hemma.env
```

On its first start Hemma creates `hemma.env` with a fresh secret that every password
depends on (only you can read the file). Keep a copy of it somewhere safe, apart from
the data folder.

It listens on port 8080 on every network address and prints where:
`Hemma is up on http://192.168.1.50:8080`. Stop it with Ctrl+C. Run `./hemma --help` for
all options.

To start it at boot without systemd, add a line to your crontab (`crontab -e`), using
full paths:

```
@reboot /home/you/hemma/hemma --data /home/you/hemma/data --env /home/you/hemma/hemma.env >> /home/you/hemma/hemma.log 2>&1
```

(Or write a service for your init system. OpenRC, runit and s6 all just need that same
command line.)

**Port 80 without root:** `sudo setcap cap_net_bind_service=+ep ~/hemma/hemma`, then set
`HEMMA_PORT=80` in `hemma.env`. Repeat the `setcap` after replacing the program. Never
run Hemma as root.
