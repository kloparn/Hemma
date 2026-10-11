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
systemctl daemon-reload

if [ ! -f "$ENVF" ]; then
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
    chown root:hemma "$ENVF"
    chmod 640 "$ENVF"
    echo
    echo "Created $ENVF with a fresh HEMMA_AUTH_SALT."
    echo "1. Start Hemma:      sudo systemctl enable --now hemma"
    echo "2. Open http://$(hostname).local (or the Pi's address) in a browser and create the admin"
    echo "   account on the welcome page. It asks for this setup code:"
    echo
    echo "        $CODE"
    echo
    echo "   (also in $ENVF). Save the recovery codes the page shows you afterwards."
    echo "3. Optional: name the machine 'hemma' so phones can open http://hemma.local"
    echo "             (on a Pi: sudo raspi-config nonint do_hostname hemma, then reboot)"
    echo "Locked out later? sudo -u hemma /opt/hemma/hemma --env $ENVF --data /var/lib/hemma --reset-password NAME"
    exit 0
fi
chown root:hemma "$ENVF"
chmod 640 "$ENVF"

systemctl enable hemma >/dev/null 2>&1 || true
systemctl restart hemma
echo "Hemma updated and restarted:  systemctl status hemma  /  journalctl -u hemma -f"
if grep -q '^HEMMA_ADMIN_PASSWORD=..*' "$ENVF"; then
    echo
    echo "Note: $ENVF still contains HEMMA_ADMIN_PASSWORD. Hemma only used it for the first admin;"
    echo "once you can sign in, clear it:  sudo sed -i 's/^HEMMA_ADMIN_PASSWORD=.*/HEMMA_ADMIN_PASSWORD=/' $ENVF"
fi
