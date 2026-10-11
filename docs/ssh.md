# SSH: talking to a computer without a screen

Your Hemma server most likely sits in a cupboard with no screen or keyboard. **SSH**
(Secure Shell) lets you type commands on it from your own computer, as if you were
sitting in front of it. Everything you type and see is encrypted.

You need to know just a little to look after Hemma: connect, run a few commands, copy a
file over. This page covers exactly that.

In the examples, `you` is your user name on the server and `hemma.local` is its name.
Use its IP address (like `192.168.1.50`) wherever a name doesn't work; see
[home-network.md](home-network.md#find-the-servers-address).

## Turn SSH on (on the server)

- **Raspberry Pi:** tick *Enable SSH* in Raspberry Pi Imager's settings when you write
  the memory card (see [raspberry-pi.md](raspberry-pi.md#2-put-raspberry-pi-os-on-the-card)).
  Already running? `sudo raspi-config` → *Interface Options* → *SSH* → *Yes*.
- **Linux:** install the SSH server:

  ```sh
  sudo apt install -y openssh-server && sudo systemctl enable --now ssh    # Debian, Ubuntu
  sudo dnf install -y openssh-server && sudo systemctl enable --now sshd   # Fedora
  ```

- **Mac:** *System Settings → General → Sharing → Remote Login* → on.
- **Windows** as the server: *Settings → System → Optional features* → add *OpenSSH
  Server*. Most Windows owners use Remote Desktop instead, which is fine.

## Connect (from your computer)

Every current Windows, Mac and Linux computer comes with the `ssh` command. Open
**PowerShell** (Windows) or **Terminal** (Mac, Linux) and type:

```sh
ssh you@hemma.local
```

**The first time,** SSH asks whether you trust this computer:

```
The authenticity of host 'hemma.local (192.168.1.50)' can't be established.
ED25519 key fingerprint is SHA256:Xb3k...
Are you sure you want to continue connecting (yes/no/[fingerprint])?
```

That's normal for a new server on your own network. Type `yes`. SSH remembers it and
won't ask again. Then type your password (nothing appears while you type; that's on
purpose). The prompt changes to something like `you@hemma:~ $`. You're in. `exit` (or
Ctrl+D) goes back to your own computer.

**"WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!"** appears if you rebuilt the server
(for example, wrote a new memory card) under the same name. If that's the case, tell your
computer to forget the old one, then connect again:

```sh
ssh-keygen -R hemma.local
```

If you *didn't* change anything on the server, stop and find out why before you type
your password.

## Keys: better than passwords

An SSH key is a pair of files: a private one that stays on your computer, and a public
one that you put on the server. With a key you don't type a password to connect, and
nobody can guess their way in.

**1. Make a key** on your own computer (once). Press Enter to accept the default place.
A passphrase is optional but a good idea on a laptop:

```sh
ssh-keygen -t ed25519
```

This creates `~/.ssh/id_ed25519` (private: never share it) and `~/.ssh/id_ed25519.pub`
(public: this one goes on servers). On Windows `~` is `C:\Users\you`.

**2. Put the public key on the server.**

- **Mac or Linux:**

  ```sh
  ssh-copy-id you@hemma.local
  ```

- **Windows** has no `ssh-copy-id`; this PowerShell line does the same:

  ```powershell
  type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh you@hemma.local "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"
  ```

- **New Raspberry Pi:** Raspberry Pi Imager can put the key on for you. Choose
  *public-key authentication* in its SSH settings and paste the contents of
  `id_ed25519.pub`.

**3. Try it:** `ssh you@hemma.local` should now connect without asking for a password.

**4. Optional, once keys work: switch passwords off** on the server, so only your keys
get in. Keep the current SSH session open while you test from a second window:

```sh
echo "PasswordAuthentication no" | sudo tee /etc/ssh/sshd_config.d/no-passwords.conf
sudo systemctl restart ssh        # "sshd" on Fedora
```

## Run one command without logging in

Put the command after the address, in quotes:

```sh
ssh you@hemma.local "systemctl status hemma"
ssh you@hemma.local "df -h"
```

Commands that need `sudo` and ask for a password need `-t`:

```sh
ssh -t you@hemma.local "sudo systemctl restart hemma"
```

## Copying files (scp)

`scp` copies files over the same connection. The side with `you@hemma.local:` is the
server; a `:` with nothing after it means your home folder there.

```sh
scp build/hemma-pi64 you@hemma.local:              # your computer -> server (home folder)
scp you@hemma.local:hemma-backup.db .              # server -> your computer (here)
scp -r you@hemma.local:some-folder .               # a whole folder (-r)
```

Run `scp` on **your own computer**, not inside an SSH session on the server.

## sudo

Normal users can't change the system. Putting `sudo` in front of a command runs it as
the administrator (root), after asking for *your* password:

```sh
sudo systemctl restart hemma
```

Only use `sudo` for the commands that need it. The guides say when.

## The commands you'll actually need

**Hemma itself** (Pi or Linux with the installer):

| Command | What it does |
|---|---|
| `systemctl status hemma` | Is it running? Shows the last few log lines too (press `q` to leave) |
| `sudo systemctl restart hemma` | Restart it, for example after changing a setting |
| `sudo systemctl stop hemma` / `start hemma` | Stop / start it |
| `journalctl -u hemma -f` | Watch the log live (Ctrl+C to stop watching). Add `sudo` if it shows nothing |
| `journalctl -u hemma -n 50` | The last 50 log lines |
| `sudo nano /etc/hemma/hemma.env` | Edit the settings (Ctrl+O, Enter to save; Ctrl+X to quit) |

**The computer:**

| Command | What it does |
|---|---|
| `df -h` | Free disk space, per disk (`h` = human-readable sizes) |
| `free -h` | Memory in use and available |
| `uptime` | How long it has been running, and how busy it is |
| `hostname -I` | Its IP address(es) |
| `sudo apt update && sudo apt full-upgrade -y` | Install system updates (Debian, Ubuntu, Raspberry Pi OS) |
| `sudo reboot` | Restart the computer (your SSH session ends; reconnect after a minute) |
| `sudo poweroff` | Shut down. Wait until the lights calm down before pulling the plug |

**Raspberry Pi only:**

| Command | What it means |
|---|---|
| `vcgencmd measure_temp` | The chip's temperature. Under about 70 °C is comfortable; at 80 °C+ it slows down to protect itself |
| `vcgencmd get_throttled` | `throttled=0x0` means all good. Anything else means power or heat trouble has happened since the last start. The usual cause is a weak power supply or cable. For example, `0x50000` means "the voltage dropped, and the Pi slowed down" |

## Staying safe

- **Use keys, not passwords**, and switch passwords off once keys work (above).
- **Keep SSH inside your home network.** Never forward port 22 (or any port) to the server
  on your router. From outside, connect through a VPN first; see
  [home-network.md](home-network.md#reaching-hemma-from-outside-the-safe-way).
- **Choose a real password** for your user, even with keys: `sudo` asks for it.
- **Keep the system updated** (`sudo apt update && sudo apt full-upgrade -y`) now and
  then.
- Your private key (`id_ed25519`, without `.pub`) is like a house key: never copy it to
  servers, chats or email.
