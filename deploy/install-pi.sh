#!/bin/sh
# deploy/install-pi.sh — install or update Hemma on a Raspberry Pi or any other Linux with systemd.
#
#   scp build/hemma-pi64 deploy/hemma.service deploy/install-pi.sh you@hemma.local:
#   ssh you@hemma.local 'sudo sh install-pi.sh ./hemma-pi64 /mnt/media'
#
# Creates a "hemma" system user, /opt/hemma/hemma, /var/lib/hemma (database),
# /etc/hemma/hemma.env (settings + secrets: root-owned, created once with a fresh
# salt) and the service. Folders after the binary are media library folders that
# admins may upload into: they become writable for Hemma (re-run with the full
# list to change them; with none, the current list is kept).
# Then it (re)starts the service, waits up to 15 s until http://127.0.0.1:<port>/healthz
# answers, and says where to open Hemma and what to do next - or, if it didn't come up,
# shows its last log lines (exit status 1).
set -eu

BIN=${1:-./hemma}
[ $# -gt 0 ] && shift
HERE=$(cd "$(dirname "$0")" && pwd)
ENVF=/etc/hemma/hemma.env
[ "$(id -u)" -eq 0 ] || { echo "run me with sudo" >&2; exit 1; }
[ -f "$BIN" ] || { echo "no binary at $BIN" >&2; exit 1; }
if ! command -v systemctl >/dev/null 2>&1 || [ ! -d /run/systemd/system ]; then
    echo "This installer needs a Linux system that runs systemd (Raspberry Pi OS, Debian, Ubuntu, Fedora, ...)." >&2
    echo "Without systemd, see docs/linux.md (\"Running without systemd\"); on a Mac, docs/macos.md." >&2
    exit 1
fi

id hemma >/dev/null 2>&1 || useradd --system --user-group --home-dir /var/lib/hemma --shell /usr/sbin/nologin hemma
install -d -o hemma -g hemma -m 750 /var/lib/hemma
install -d -m 755 /opt/hemma
# the program it replaces stays as hemma.prev: the way back if the new one misbehaves
if [ -f /opt/hemma/hemma ] && ! { command -v cmp >/dev/null 2>&1 && cmp -s "$BIN" /opt/hemma/hemma; }; then
    cp -p /opt/hemma/hemma /opt/hemma/hemma.prev
fi
install -m 755 "$BIN" /opt/hemma/hemma
install -d -o root -g hemma -m 750 /etc/hemma
install -m 644 "$HERE/hemma.service" /etc/systemd/system/hemma.service

# media folders for uploads: writable for the hemma user + allowed by the service
if [ $# -gt 0 ]; then
    RW=""
    for DIR in "$@"; do
        case "$DIR" in /*) ;; *) echo "media folders need absolute paths: $DIR" >&2; exit 1 ;; esac
        # (", \ and % mean something in the systemd drop-in these paths go into)
        case "$DIR" in *'"'* | *'\'* | *%*) echo "unsupported character (\", \\ or %) in: $DIR" >&2; exit 1 ;; esac
        [ -d "$DIR" ] || { echo "no folder at $DIR (mount the disk first)" >&2; exit 1; }
        FS=$(findmnt -no FSTYPE --target "$DIR" 2>/dev/null || echo unknown)
        case "$FS" in
        ntfs | ntfs3 | fuseblk | exfat | vfat | msdos)
            # no Linux permissions on these: ownership comes from the mount options
            echo "$DIR is on $FS. Mount it as the hemma user so uploads can be written, e.g. in /etc/fstab:"
            echo "    UUID=<disk uuid>  $DIR  $FS  uid=$(id -u hemma),gid=$(id -g hemma),umask=0022,nofail  0  0"
            echo "  (find the UUID with: lsblk -f)"
            ;;
        *)
            if command -v setfacl >/dev/null 2>&1; then
                setfacl -R -m u:hemma:rwX -m d:u:hemma:rwX "$DIR"
                echo "Hemma may now write into $DIR (ACLs; existing files keep their owners)."
            else
                chgrp hemma "$DIR" && chmod g+rwx "$DIR"
                echo "Hemma may now write into $DIR itself. For existing sub-folders too: sudo apt install acl, re-run."
            fi
            ;;
        esac
        RW="$RW \"-$DIR\""
    done
    install -d -m 755 /etc/systemd/system/hemma.service.d
    printf '# Media library folders Hemma may write to (uploads). Written by install-pi.sh.\n[Service]\nReadWritePaths=%s\n' \
        "$RW" >/etc/systemd/system/hemma.service.d/media.conf
fi
# The clock: a Pi has no battery clock, so after a power cut "today", clash checks and the TV
# channels' clock can be hours off until NTP answers. hemma.service waits for time-sync.target,
# which only waits for the clock when systemd-time-wait-sync is enabled - at most 90 s here, so
# a day without internet still starts Hemma (the wait then shows as failed in systemctl).
if systemctl cat systemd-time-wait-sync.service >/dev/null 2>&1; then
    install -d -m 755 /etc/systemd/system/systemd-time-wait-sync.service.d
    {
        echo "# Written by Hemma's install-pi.sh: wait for the clock at boot, but at most 90 s."
        echo "[Service]"
        echo "TimeoutStartSec=90"
    } >/etc/systemd/system/systemd-time-wait-sync.service.d/hemma.conf
    systemctl enable systemd-time-wait-sync.service >/dev/null 2>&1 || true
fi
systemctl daemon-reload

FIRST=0
CODE=""
if [ ! -f "$ENVF" ]; then
    FIRST=1
    SALT=$(/opt/hemma/hemma --gen-salt)
    CODE=$(/opt/hemma/hemma --gen-setup-code)
    umask 027
    cat >"$ENVF" <<EOF
# Hemma settings (see .env.example in the repository). Root-owned: the service reads it
# but can't change it. Edit with: sudo nano $ENVF
HEMMA_AUTH_SALT=$SALT
# The one-time code the welcome page asks for: proof that whoever creates the admin can see
# this machine, not just anyone on the Wi-Fi. Useless once the admin exists.
HEMMA_SETUP_CODE=$CODE
# The first admin is created in the browser: the first visit shows a welcome page.
# Headless instead? Set both lines before the first start; the password is used only
# that once - remove it after you've signed in (see docs/accounts.md).
# HEMMA_ADMIN_USER=admin
# HEMMA_ADMIN_PASSWORD=
HEMMA_PORT=80
EOF
    echo
    echo "Created $ENVF with a fresh HEMMA_AUTH_SALT and a one-time setup code."
fi
chown root:hemma "$ENVF"
chmod 640 "$ENVF"

# ---- start it (again), and wait until it answers -----------------------------------------
setting() { # $1's value as Hemma reads it: the first line with a value, without quotes or " # note"
    sed -n "s/^[[:space:]]*\(export[[:space:]][[:space:]]*\)\{0,1\}$1[[:space:]]*=[[:space:]]*//p" "$ENVF" |
        tr -d '\r' | sed 's/^#.*//; s/^\([^"'\''].*\)[[:space:]]#.*$/\1/; s/[[:space:]]*$//' |
        sed 's/^"\(.*\)"$/\1/; s/^'\''\(.*\)'\''$/\1/' | sed -n '/./{p;q;}'
}
PORT=$(setting HEMMA_PORT)
PORT=${PORT:-8080}
HOST=$(setting HEMMA_HOST)
case "$HOST" in "" | 0.0.0.0) PROBE=127.0.0.1 ;; *) PROBE=$HOST ;; esac
SUFFIX=""
[ "$PORT" = 80 ] || SUFFIX=":$PORT"
# GET /healthz answers "ok" once Hemma serves pages (curl or wget; without either, the
# service staying up for a few seconds has to do)
if command -v curl >/dev/null 2>&1; then
    FETCH="curl -fsS -m 2"
elif command -v wget >/dev/null 2>&1; then
    FETCH="wget -q -T 2 -O -"
else
    FETCH=""
fi
answers() { [ -n "$FETCH" ] && $FETCH "http://$PROBE:$PORT/healthz" 2>/dev/null | grep -q '^ok'; }

# The database as it was, before the new program first opens it (a new data format can mean an older
# Hemma refuses it): hemma.db.before-install-<when>, the newest 3 kept. Stopped first, so it's whole.
DATA=/var/lib/hemma
if [ -f "$DATA/hemma.db" ]; then
    systemctl stop hemma >/dev/null 2>&1 || true
    STAMP=$(date '+%Y-%m-%d-%H%M%S')
    cp -p "$DATA/hemma.db" "$DATA/hemma.db.before-install-$STAMP"
    [ ! -f "$DATA/hemma.db-wal" ] || cp -p "$DATA/hemma.db-wal" "$DATA/hemma.db.before-install-$STAMP-wal"
    ls -1 "$DATA"/hemma.db.before-install-* 2>/dev/null | grep -v -- '-wal$' | sort -r | tail -n +4 |
        while read -r OLD; do rm -f "$OLD" "$OLD-wal"; done
    echo
    echo "Kept a copy of the database as $DATA/hemma.db.before-install-$STAMP"
fi

STARTED_AT=$(date '+%Y-%m-%d %H:%M:%S')
systemctl enable hemma >/dev/null 2>&1 || true
systemctl restart hemma || true
echo
echo "Starting Hemma..."
UP=0
ACTIVE=0
i=0
while [ "$i" -lt 15 ]; do
    sleep 1
    i=$((i + 1))
    if systemctl is-active --quiet hemma; then ACTIVE=$((ACTIVE + 1)); else ACTIVE=0; fi
    [ "$ACTIVE" -gt 0 ] || continue
    if [ -n "$FETCH" ]; then
        if answers; then UP=1; break; fi
    elif [ "$ACTIVE" -ge 4 ]; then
        UP=1
        break
    fi
done

if [ "$UP" -ne 1 ]; then
    echo
    echo "Hemma did NOT start properly. Its last log lines:"
    echo
    journalctl -u hemma -n 30 --no-pager 2>/dev/null || systemctl status hemma --no-pager 2>/dev/null || true
    echo
    echo "The usual causes:"
    echo "  - another program already uses port $PORT     (see: sudo ss -ltnp | grep ':$PORT ')"
    echo "  - a mistake in the settings file               (sudo nano $ENVF)"
    echo "  - a media folder that isn't there (not mounted?) - the lines above say"
    echo "  - data it won't start on (a newer Hemma's, or damaged): the lines above say why, and"
    echo "    Hemma doesn't retry. Going back: docs/raspberry-pi.md, \"Going back\""
    echo "Fix it, then: sudo systemctl restart hemma   (or run this installer again)."
    echo "More help: docs/troubleshooting.md"
    exit 1
fi

# ---- it's running: where to open it, what to do next, and the handy commands ---------------
lan_ips() { # this machine's IPv4 addresses on the network (no loopback)
    { hostname -I 2>/dev/null || ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}'; } |
        tr ' ' '\n' | sed 's#/.*##' | grep -E '^[0-9]+(\.[0-9]+){3}$' | grep -v '^127\.' || true
}
case "$HOST" in "" | 0.0.0.0) IPS=$(lan_ips) ;; *) IPS=$HOST ;; esac
NAME=$(hostname 2>/dev/null | cut -d. -f1 || true)
# still waiting for its first admin? Hemma logs the welcome page's setup code at start
sleep 1
SETUP=$(journalctl -u hemma --since "$STARTED_AT" --no-pager -o cat 2>/dev/null |
    sed -n 's/.*asks for this setup code: \([0-9A-Z][0-9A-Z-]*\).*/\1/p' | tail -n 1 || true)
[ -z "$SETUP" ] && [ "$FIRST" -eq 1 ] && SETUP=$CODE

echo
echo "Hemma is running."
echo
echo "Open it on a phone or computer at home:"
for IP in $IPS; do echo "    http://$IP$SUFFIX"; done
if [ -n "$NAME" ] && systemctl is-active --quiet avahi-daemon 2>/dev/null; then
    echo "    http://$NAME.local$SUFFIX    (phones and computers; a TV may need the number)"
fi
[ -n "$IPS" ] || echo "    (no network address found - check the cable or Wi-Fi, then: hostname -I)"
echo
echo "Next steps:"
n=1
if [ -n "$SETUP" ]; then
    echo "  $n. Open the address above and create the admin account. The welcome page asks for"
    echo "     this setup code (it's also in $ENVF):"
    echo
    echo "         $SETUP"
    echo
    n=$((n + 1))
    echo "  $n. Save the recovery codes Hemma shows you afterwards."
    n=$((n + 1))
fi
echo "  $n. In Hemma, open System (under More on a phone): it checks that everything works,"
echo "     shows the log and lists what to set up next."
n=$((n + 1))
echo "  $n. Give this machine a fixed address in your router, so bookmarks keep working"
echo "     (docs/home-network.md)."
if [ "$FIRST" -eq 1 ]; then
    echo "  Optional: name the machine 'hemma' so phones can open http://hemma.local"
    echo "            (on a Pi: sudo raspi-config nonint do_hostname hemma, then reboot)"
fi
echo
echo "Handy commands:"
echo "  systemctl status hemma          is it running? (and its last lines)"
echo "  journalctl -u hemma -f          follow the log (Ctrl+C stops watching)"
echo "  sudo systemctl restart hemma    restart it, e.g. after changing the settings"
echo "  sudo nano $ENVF   the settings"
echo "  Update: copy the new program over, then run this installer again with it."
echo "  Going back: sudo sh -c 'cd /opt/hemma && [ -f hemma.prev ] && mv hemma hemma.x && mv hemma.prev hemma && mv hemma.x hemma.prev'"
echo "              then sudo systemctl restart hemma  (more: docs/raspberry-pi.md, \"Going back\")"
echo "  Data: /var/lib/hemma (the database hemma.db, copies hemma.db.before-*, Hemma's own log hemma.log)"
echo "  Locked out? sudo -u hemma /opt/hemma/hemma --env $ENVF --data /var/lib/hemma --reset-password NAME"
echo "  Something wrong? docs/troubleshooting.md"
if grep -q '^HEMMA_ADMIN_PASSWORD=..*' "$ENVF"; then
    echo
    echo "Note: $ENVF still contains HEMMA_ADMIN_PASSWORD. Hemma only used it for the first admin;"
    echo "once you can sign in, clear it:  sudo sed -i 's/^HEMMA_ADMIN_PASSWORD=.*/HEMMA_ADMIN_PASSWORD=/' $ENVF"
fi
