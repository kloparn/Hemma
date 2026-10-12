# Hemma on a Mac

A Mac that stays on (a Mac mini in a cupboard, an old MacBook) can be the household's
Hemma server.

> **macOS support is new.** Hemma builds for macOS 11 (Big Sur) and newer, on both Apple
> silicon and Intel, and it should run. It hasn't been tested much on real Macs yet, so
> if something's off, please tell us. The parts most likely to need a fix are the
> **Network** page (finding devices) and anything to do with permissions.

Commands are for the **Terminal** app (Applications → Utilities → Terminal).

## 1. Get the program

### Build it on the Mac (recommended)

Install Apple's command-line tools (the compiler, `make` and `git`) once:

```sh
xcode-select --install
```

Then get the source code and build:

```sh
git clone <project URL> hemma      # or download the ZIP from the project page and unzip it
cd hemma
make            # -> build/hemma (the first build compiles SQLite: a minute or two)
make test       # optional: runs the test suite
```

> **TODO (owner):** put the project's public address here once it's published.

If you also have [zig](https://ziglang.org) (`brew install zig`), `make` adds the
in-browser video converter. Without it, uploads simply send the original files.

### Or build it on a Windows PC

`.\build.ps1 mac` (see [windows.md](windows.md)) makes two files:

| Your Mac | File |
|---|---|
| Apple silicon (M1, M2, …): *About This Mac* says "Chip: Apple M…" | `hemma-mac-arm64` |
| Intel: *About This Mac* says "Processor: … Intel …" | `hemma-mac-x64` |

Copy the right one to the Mac and rename it `hemma`. Files that arrive by browser
download, AirDrop or mail are marked as "from the internet", and macOS refuses to run
them. Clear the mark and make the file runnable:

```sh
xattr -d com.apple.quarantine hemma
chmod +x hemma
```

## 2. Set it up in its own folder

Keep the program, its settings and its database together in `~/Hemma`:

```sh
mkdir -p ~/Hemma
cp build/hemma ~/Hemma/          # or the file you copied over
cd ~/Hemma
```

On its first start Hemma creates the settings file `hemma.env` with a fresh secret that
every password depends on (only you can read the file). Keep a copy of it somewhere safe,
apart from the data folder. More settings are listed in [.env.example](../.env.example).

Start it:

```sh
~/Hemma/hemma --data ~/Hemma/data --env ~/Hemma/hemma.env
```

It prints where it's listening, for example `Hemma is up on http://192.168.1.50:8080`.
Open `http://localhost:8080` on the Mac and **create the admin account**. The welcome
page asks for the **setup code** Hemma printed in its window when it started (`The
welcome page asks for this setup code: 7KQ4-M2XP`). More in [accounts.md](accounts.md).
Stop it with Ctrl+C.

Other devices use the Mac's name or address:

- **Name:** *System Settings → General → Sharing*. The *Local hostname* at the bottom
  is something like `Your-Mac-mini.local`, so the address is
  `http://Your-Mac-mini.local:8080`.
- **Address:** `ipconfig getifaddr en0` in Terminal (or *System Settings → Wi-Fi →
  Details*). Then use `http://192.168.1.50:8080`.

### The firewall

The macOS firewall is off by default. If you've turned it on, macOS asks whether *hemma*
may accept incoming network connections. Choose **Allow**. If you missed it, or Hemma
runs in the background (next step): *System Settings → Network → Firewall → Options*,
press **+**, choose `~/Hemma/hemma` (Cmd+Shift+G lets you type the path), and set it to
*Allow incoming connections*. After replacing the program with a new version, you may
need to allow it again.

**A MacBook that leaves the house?** Hemma answers on every network the Mac joins: at a
café, the office or a hotel, everyone on that Wi-Fi could open your sign-in page. The
macOS firewall can't tell your home network from others, so either keep Hemma on a Mac
that stays home, or don't start it at login (skip step 3) and start it by hand when
you're home: `~/Hemma/hemma --data ~/Hemma/data --env ~/Hemma/hemma.env`.

### Other permissions

- **Movies in Documents, Desktop or Downloads, or on an external disk:** macOS may hide
  those from a background program. If Hemma's library stays empty, add `~/Hemma/hemma`
  under *System Settings → Privacy & Security → Full Disk Access*. Or keep movies in a
  folder like `~/Movies`.
- **The Network page finds nothing** (macOS 15 and newer): check *System Settings →
  Privacy & Security → Local Network* and allow Hemma (or Terminal, if you started it
  from there).

## 3. Start Hemma when you log in

`deploy/local.hemma.plist` is a ready-made launchd agent. It starts Hemma at login, starts
it again if it stops, and writes the log to `~/Hemma/hemma.log`. From the source folder:

```sh
sed "s|/Users/YOU|$HOME|g" deploy/local.hemma.plist > ~/Library/LaunchAgents/local.hemma.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.hemma.plist
```

(The first line puts your own home folder into the file.) Day to day:

```sh
tail -f ~/Hemma/hemma.log                          # the log (Ctrl+C to stop watching)
launchctl kickstart -k gui/$(id -u)/local.hemma    # restart (e.g. after an update)
launchctl bootout gui/$(id -u)/local.hemma         # stop until next login
rm ~/Library/LaunchAgents/local.hemma.plist        # ...and then this: no more autostart
```

**Updating:** copy the new program over `~/Hemma/hemma`, then restart it (above).

In Hemma, admins open **System**: checks in plain words and the log, live (Hemma also keeps
it in `~/Hemma/data/hemma.log`). When something's wrong: [troubleshooting.md](troubleshooting.md).

An agent only runs while you're logged in. For a Mac that sits in a cupboard, turn on
automatic login (*System Settings → Users & Groups → Automatically log in as*). The
alternative is a launch *daemon*, which runs from startup without anyone logged in.
Copy the plist to `/Library/LaunchDaemons/` (owned by root), add a `UserName` key with
your user name so Hemma doesn't run as root, and load it with
`sudo launchctl bootstrap system /Library/LaunchDaemons/local.hemma.plist`.

## 4. Keep the Mac awake

A sleeping Mac can't answer phones.

- **Desktop Macs (Mac mini, iMac, Studio):** *System Settings → Energy*: turn on
  **Prevent automatic sleeping when the display is off** and **Wake for network access**.
- **Laptops:** *System Settings → Battery → Options*: turn on **Prevent automatic
  sleeping on power adapter when the display is off**. A closed lid still means sleep,
  unless an external display is connected.
- **From Terminal:** `sudo pmset -c sleep 0` means never sleep on the power adapter
  (`pmset -g` shows the current settings). Or let Hemma keep the Mac awake while it
  runs on the power adapter:
  `caffeinate -s ~/Hemma/hemma --data ~/Hemma/data --env ~/Hemma/hemma.env`. In the
  plist, add `<string>/usr/bin/caffeinate</string><string>-s</string>` as the first two
  entries of `ProgramArguments`.

## Movies and backups

- **Movies:** in Hemma, open **Watch → Library settings** and enter the folders, one per
  line, for example `/Users/you/Movies` or `/Volumes/MyDisk/Media`.
- **Backups:** the database is `~/Hemma/data/hemma.db`. Stop Hemma, copy the `data`
  folder (Time Machine does this too), start it again. Keep `hemma.env` (the salt)
  separate and safe. The [Pi guide](raspberry-pi.md#backups) explains why.

## Good to know

- Video streams on a Mac go through a small buffer instead of the zero-copy path Linux
  uses. For a household that makes no noticeable difference.
- The **Network** page reads the Mac's routing and ARP tables. That code is new; if
  devices or "who's home" look wrong on a Mac, please report it.
