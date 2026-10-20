#!/bin/sh
# tools/image/build.sh — the ready-made Raspberry Pi image (issue #71): write it to a memory card or USB
# stick with Raspberry Pi Imager, plug the Pi in, open http://hemma.local.
#
#   sh tools/image/build.sh hemma-pi64.tar.gz OUT_DIR      (run from the repo's top; needs no root)
#
# It is the official Raspberry Pi OS Lite (64-bit: Pi 3, 4, 5, Zero 2) with files added, and nothing in
# it changed or run: the build writes into the image's ext4 partition with debugfs, as a normal user, on
# any Linux (CI, WSL). What it adds:
#   /opt/hemma-image/hemma-pi64/   the Pi download (its installer, units and helpers, signed in CI)
#   /opt/hemma-image/debs/         unattended-upgrades and what it needs that the image lacks, from
#                                  Debian, checked against Debian's signatures (InRelease, with the
#                                  keyring inside the image itself) - installed offline at first start
#   /opt/hemma-image/firstboot.sh  + hemma-firstboot.service (enabled): at the first start, after
#                                  Raspberry Pi Imager's settings, names the machine hemma (unless Imager
#                                  did) and runs Hemma's own installer - so the password secret and the
#                                  setup code are made on that Pi, never in the image
# Nothing secret goes in: no users, no passwords, no SSH host keys (Raspberry Pi OS makes them at the
# first start), SSH stays off unless Imager turns it on, /etc/machine-id stays unset.
#
# HEMMA_IMAGE_BASE: the base image (default Raspberry Pi's newest Lite arm64; its .sha256 beside it is
# checked). HEMMA_IMAGE_CACHE: a folder to keep the base download in between builds.
set -eu
bundle=${1:?the Pi download: hemma-pi64.tar.gz}
out=${2:?output folder}
ver=$(tr -d ' \r\n' <VERSION)
here=$(cd "$(dirname "$0")" && pwd)
base_url=${HEMMA_IMAGE_BASE:-https://downloads.raspberrypi.com/raspios_lite_arm64_latest}
debian=https://deb.debian.org/debian
suite=trixie
want="unattended-upgrades acl"

for c in curl xz sfdisk dd debugfs e2fsck gpgv sha256sum awk tar; do
    command -v "$c" >/dev/null 2>&1 || { echo "build.sh needs $c" >&2; exit 1; }
done
mkdir -p "$out"
out=$(cd "$out" && pwd)
bundle=$(cd "$(dirname "$bundle")" && pwd)/$(basename "$bundle")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
say() { printf '%s\n' "$*"; }

# ---- the base image, checked
url=$(curl -sSfL -o /dev/null -w '%{url_effective}' -I "$base_url")
name=$(basename "$url")
case $name in *.img.xz) ;; *) echo "the base isn't an .img.xz: $url" >&2; exit 1 ;; esac
store=${HEMMA_IMAGE_CACHE:-$work}
mkdir -p "$store"
[ -f "$store/$name" ] || { say "downloading $name"; curl -sSfL -o "$store/$name.part" "$url" && mv "$store/$name.part" "$store/$name"; }
curl -sSfL -o "$work/base.sha256" "$url.sha256"
want_sum=$(awk '{print $1; exit}' "$work/base.sha256")
[ "$(sha256sum "$store/$name" | awk '{print $1}')" = "$want_sum" ] || { echo "$name: its SHA-256 doesn't match Raspberry Pi's" >&2; exit 1; }
say "base: $name (SHA-256 checked)"
xz -dc -T0 "$store/$name" >"$work/hemma.img"

# ---- its root file system, taken out to work on
img=$work/hemma.img
set -- $(sfdisk -d "$img" | sed -n "s#^$img"'2 : start= *\([0-9]*\), size= *\([0-9]*\),.*#\1 \2#p')
[ $# -eq 2 ] || { echo "no second partition in $name" >&2; exit 1; }
start=$1 sectors=$2
root=$work/root.ext4
dd if="$img" of="$root" bs=4M iflag=skip_bytes,count_bytes skip=$((start * 512)) count=$((sectors * 512)) status=none
e2fsck -fn "$root" >/dev/null 2>&1 || { echo "the base's root file system isn't clean" >&2; exit 1; }

# ---- what Debian adds (security updates by themselves), checked against Debian's own signatures
debugfs -R "dump /usr/share/keyrings/debian-archive-keyring.pgp $work/debian.pgp" "$root" 2>/dev/null
debugfs -R "dump /var/lib/dpkg/status $work/status" "$root" 2>/dev/null
[ -s "$work/debian.pgp" ] && [ -s "$work/status" ] || { echo "no Debian keyring or package list in the image" >&2; exit 1; }
curl -sSfL -o "$work/InRelease" "$debian/dists/$suite/InRelease"
gpgv --keyring "$work/debian.pgp" "$work/InRelease" 2>"$work/gpgv.log" || { cat "$work/gpgv.log" >&2; echo "Debian's InRelease didn't verify" >&2; exit 1; }
idx_sum=$(sed -n '/^-----BEGIN PGP SIGNED MESSAGE-----/,/^-----BEGIN PGP SIGNATURE-----/p' "$work/InRelease" |
    awk '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && $3 == "main/binary-arm64/Packages.xz" { print $1; exit }')
[ -n "$idx_sum" ] || { echo "Debian's InRelease names no arm64 package index" >&2; exit 1; }
curl -sSfL -o "$work/Packages.xz" "$debian/dists/$suite/main/binary-arm64/Packages.xz"
[ "$(sha256sum "$work/Packages.xz" | awk '{print $1}')" = "$idx_sum" ] || { echo "Debian's package index doesn't match its InRelease" >&2; exit 1; }
xz -dc "$work/Packages.xz" >"$work/Packages"
awk -v want="$want" -f "$here/debs.awk" "$work/status" "$work/Packages" >"$work/debs.list"
mkdir "$work/debs"
while read -r pkg file sum size; do
    f=$work/debs/$(basename "$file")
    curl -sSfL -o "$f" "$debian/$file"
    [ "$(sha256sum "$f" | awk '{print $1}')" = "$sum" ] || { echo "$file doesn't match Debian's index" >&2; exit 1; }
    say "  + $pkg ($(basename "$file"), $size bytes)"
done <"$work/debs.list"

# ---- Hemma's Pi download
mkdir "$work/b"
tar -xzf "$bundle" -C "$work/b"
pi=$work/b/hemma-pi64
[ -f "$pi/hemma-pi64" ] && [ -f "$pi/install-pi.sh" ] || { echo "$bundle isn't Hemma's Pi download" >&2; exit 1; }
printf 'Hemma %s - the ready-made image, built from %s\n' "$ver" "$name" >"$work/README.txt"

# ---- into the image (debugfs: files owned by root, the modes set here)
cmds=$work/debugfs.cmds
{
    echo "mkdir /opt/hemma-image"
    echo "mkdir /opt/hemma-image/hemma-pi64"
    echo "mkdir /opt/hemma-image/debs"
    for d in /opt/hemma-image /opt/hemma-image/hemma-pi64 /opt/hemma-image/debs; do
        printf 'sif %s mode 040755\nsif %s uid 0\nsif %s gid 0\n' "$d" "$d" "$d"
    done
    put() { # local, folder, name, mode
        printf 'cd %s\nwrite %s %s\nsif %s mode %s\nsif %s uid 0\nsif %s gid 0\n' "$2" "$1" "$3" "$3" "$4" "$3" "$3"
    }
    put "$here/firstboot.sh" /opt/hemma-image firstboot.sh 0100755
    put "$work/README.txt" /opt/hemma-image README.txt 0100644
    for f in "$pi"/*; do
        case $(basename "$f") in hemma-pi64 | install-pi.sh | hemma-upgrade | hemma-disks) m=0100755 ;; *) m=0100644 ;; esac
        put "$f" /opt/hemma-image/hemma-pi64 "$(basename "$f")" $m
    done
    for f in "$work"/debs/*.deb; do put "$f" /opt/hemma-image/debs "$(basename "$f")" 0100644; done
    put "$here/hemma-firstboot.service" /etc/systemd/system hemma-firstboot.service 0100644
    echo "cd /etc/systemd/system/multi-user.target.wants"
    echo "symlink hemma-firstboot.service /etc/systemd/system/hemma-firstboot.service"
} >"$cmds"
debugfs -w -f "$cmds" "$root" >"$work/debugfs.log" 2>&1
if grep -Ei 'error|could not|not found|exists' "$work/debugfs.log" | grep -v '^debugfs:' >"$work/debugfs.err"; then
    cat "$work/debugfs.err" >&2
    echo "debugfs couldn't add everything" >&2
    exit 1
fi
e2fsck -fn "$root" >"$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; echo "the root file system isn't clean after the changes" >&2; exit 1; }
debugfs -R "stat /etc/systemd/system/multi-user.target.wants/hemma-firstboot.service" "$root" 2>/dev/null | grep -q 'Type: symlink' ||
    { echo "hemma-firstboot.service isn't enabled in the image" >&2; exit 1; }
[ -z "$(debugfs -R 'ls /etc/ssh' "$root" 2>/dev/null | grep -o 'ssh_host_[a-z0-9_]*key')" ] || { echo "SSH host keys in the image" >&2; exit 1; }

# ---- back into place, packed
dd if="$root" of="$img" bs=4M oflag=seek_bytes seek=$((start * 512)) conv=notrunc status=none
file=$out/hemma-$ver-pi.img.xz
xz -T0 -6 -c "$img" >"$file.part"
mv "$file.part" "$file"
say "$file ($(du -h "$file" | cut -f1)): Hemma $ver on $name"
