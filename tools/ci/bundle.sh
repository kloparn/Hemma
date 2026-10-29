#!/bin/sh
# tools/ci/bundle.sh — the downloads of a release (.github/workflows/release.yml; issue #19): a bundle
# per platform - a Pi or Linux one holds the installer and the service file the installer copies from
# its own folder, and a tarball keeps the program executable. No Windows download yet (Defender's
# machine learning flags hemma.exe: release.yml). Plus SHA256SUMS over all of them, and the release
# notes (this version's CHANGELOG.md section, which must be there, how to start each download, and
# the commits since the previous release - the newest v* tag before HEAD, or HEMMA_PREV).
# SIGNED (issue #73): with HEMMA_SIGNER = a hemma program that runs here and HEMMA_SIGNING_KEY = the
# project's secret key, every SHA256SUMS gets its SHA256SUMS.sig (hemma --sign) - each download's own
# and the release's. Installed Hemmas take nothing else (hemma-upgrade, hemma --verify-release);
# without them (a dry run, a local try) nothing is signed.
#   sh tools/ci/bundle.sh PROGRAMS_DIR OUT_DIR [NOTES]   (PROGRAMS_DIR: what ./build.ps1 pi, pi32,
#                                                       linux and mac made;
#                                                       NOTES: where the release notes go, notes.md)
set -eu
in=${1:?programs folder}
out=${2:?output folder}
notes=${3:-notes.md}
ver=$(tr -d ' \r\n' < VERSION)
echo "$ver" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+' || { echo "VERSION must hold MAJOR.MINOR.PATCH: $ver"; exit 1; }
repo=https://github.com/kloparn/Hemma
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d) # (a runner's, gone with it)

# this version's section of CHANGELOG.md: what the release says changed
awk -v v="$ver" '
    /^## / { if (on) exit; if ($2 == v) { on = 1; next } }
    on' CHANGELOG.md > "$work/changes.md"
grep -q '[^[:space:]]' "$work/changes.md" || { echo "CHANGELOG.md has no section for $ver"; exit 1; }

# SHA256SUMS.sig beside a folder's SHA256SUMS (and a check that the installed Hemmas will take it)
sign() {
    [ -n "${HEMMA_SIGNER:-}" ] || return 0
    "$HEMMA_SIGNER" --sign "$1/SHA256SUMS" > "$1/SHA256SUMS.sig"
    "$HEMMA_SIGNER" --verify-release "$1" > /dev/null
}

# name, program, what goes with it, and the few lines that say how to start it
pack() {
    name=$1 prog=$2 how=$3
    shift 3
    d=$work/$name
    mkdir -p "$d"
    cp "$in/$prog" "$d/$prog"
    for f in "$@"; do cp "$f" "$d/"; done
    cp LICENSE "$d/"
    cp third_party/ffmpeg/COPYING.LGPLv2.1 "$d/LICENSE-LGPL-2.1.txt" # (FFmpeg's two DTS tables are under it)
    echo "$ver" > "$d/VERSION" # (signed with the rest: hemma-upgrade reads the version here)
    chmod 644 "$d"/* # (whatever the checkout says - a Windows drive calls everything executable)
    chmod 755 "$d/$prog"
    for x in install-pi.sh hemma-upgrade hemma-disks; do if [ -f "$d/$x" ]; then chmod 755 "$d/$x"; fi; done
    printf 'Hemma %s - %s\n\n%s\n\nGuides: %s/tree/v%s/docs\n\nLicence: Apache 2.0 (LICENSE). Two tables in the program, from FFmpeg, are under the\nGNU LGPL 2.1 or later (LICENSE-LGPL-2.1.txt). The source of all of it: %s/tree/v%s\n' \
        "$ver" "$name" "$how" "$repo" "$ver" "$repo" "$ver" > "$d/README.txt"
    (cd "$d" && sha256sum -- * > SHA256SUMS)
    sign "$d"
    tar -C "$work" -czf "$out/$name.tar.gz" "$name"
}

pi='Install (on the Pi):  sudo sh install-pi.sh ./PROG
Media folders the uploads may write to can follow: sudo sh install-pi.sh ./PROG /srv/media
It keeps the program it replaces as /opt/hemma/hemma.prev (going back: docs/raspberry-pi.md).
Later updates: sudo hemma-upgrade (installed with it), or System -> Updates in Hemma.'
units="deploy/install-pi.sh deploy/hemma.service deploy/hemma-setup.service deploy/hemma-setup.path
       deploy/hemma-upgrade deploy/hemma-update.service deploy/hemma-update.path
       deploy/hemma-disks deploy/hemma-disks.service deploy/hemma-disks.path deploy/avahi-hemma.service"
# shellcheck disable=SC2086 # (the list splits on purpose)
pack hemma-pi64 hemma-pi64 "$(echo "$pi" | sed 's/PROG/hemma-pi64/g')" $units
# shellcheck disable=SC2086
pack hemma-pi32 hemma-pi32 "$(echo "$pi" | sed 's/PROG/hemma-pi32/g')" $units
# shellcheck disable=SC2086
pack hemma-linux-x64 hemma-linux "$(echo "$pi" | sed 's/PROG/hemma-linux/g; s/on the Pi/any systemd Linux/')" $units
mac='Try it:  ./PROG --demo   (a made-up household), or just ./PROG and open http://localhost:8080
macOS blocks programs from the internet that Apple has not checked. Once:
  xattr -d com.apple.quarantine PROG      (or right-click it in Finder -> Open)
To keep it running: docs/macos.md (local.hemma.plist is here).'
pack hemma-mac-arm64 hemma-mac-arm64 "$(echo "$mac" | sed 's/PROG/hemma-mac-arm64/g')" deploy/local.hemma.plist
pack hemma-mac-x64 hemma-mac-x64 "$(echo "$mac" | sed 's/PROG/hemma-mac-x64/g')" deploy/local.hemma.plist

# the ready-made Raspberry Pi image (issue #71): with HEMMA_IMAGE=1 (CI) - a 550 MB download and a minute
# or two; tools/image/build.sh says what's in it
img=""
if [ "${HEMMA_IMAGE:-}" = 1 ]; then
    sh tools/image/build.sh "$out/hemma-pi64.tar.gz" "$out"
    img=hemma-$ver-pi.img.xz
    [ -f "$out/$img" ] || { echo "the image wasn't made"; exit 1; }
fi
# the Android app (2.0; apps/android): when the build made it (signed with the project's app key: release.yml) -
# the same app every program in the release serves (Your account -> Get the app for Android)
apk=""
if [ -f "$in/hemma.apk" ]; then
    apk=hemma-android.apk
    cp "$in/hemma.apk" "$out/$apk"
fi
# shellcheck disable=SC2086 # ($img, $apk: one name or none)
(cd "$out" && sha256sum -- *.tar.gz $img $apk > SHA256SUMS)
sign "$out"
apk_row="" apk_note=""
if [ -n "$apk" ]; then
    apk_row="| An Android phone: the Hemma app | \`$apk\` |"
    apk_note="
**The Android app:** your Hemma offers it itself (Household → Your account → *Get the app for Android*),
or take \`$apk\` here. It finds Hemma on your home Wi-Fi by itself and plays audiobooks with the phone
locked; [docs/android.md]($repo/blob/v$ver/docs/android.md) tells more.
"
fi

# the image's line in the downloads table and how to use it (only when it was made)
image_row="" image_note=""
if [ -n "$img" ]; then
    image_row="| A new Raspberry Pi, ready to go (Pi 3, 4, 5, Zero 2) | \`$img\` |"
    image_note="
**A new Pi, no terminal at all:** write \`$img\` to a memory card or USB stick with Raspberry Pi
Imager (*Choose OS → Use custom*), put it in the Pi, plug it in, and open http://hemma.local a few
minutes later. The password secret and the setup code are made on your Pi at its first start;
[docs/raspberry-pi.md]($repo/blob/v$ver/docs/raspberry-pi.md) has the details. Only the newest
release has an image: an older one is taken away when a new release comes out.
"
fi

# (the image's and the app's rows, without a blank line between: that would end the table)
more_rows=$(printf '%s\n%s\n' "$image_row" "$apk_row" | sed '/^$/d')

# the notes: what changed, then the downloads
{
    cat "$work/changes.md"
    cat <<EOF

## Downloads

| | |
|---|---|
| Raspberry Pi, 64-bit OS (Pi 3, 4, 5, Zero 2) | \`hemma-pi64.tar.gz\` |
| Raspberry Pi, 32-bit OS (every Pi) | \`hemma-pi32.tar.gz\` |
| Linux PC or server (x86-64) | \`hemma-linux-x64.tar.gz\` |
| Mac with Apple silicon / Intel | \`hemma-mac-arm64.tar.gz\` / \`hemma-mac-x64.tar.gz\` |
$more_rows
$image_note$apk_note
**On a Pi:**
\`\`\`sh
wget $repo/releases/download/v$ver/hemma-pi64.tar.gz
tar xzf hemma-pi64.tar.gz && cd hemma-pi64
sudo sh install-pi.sh ./hemma-pi64
\`\`\`
**Updating** a Hemma installed so (0.21.1 or newer): \`sudo hemma-upgrade\`, or System → Updates in Hemma.

**Try it on a laptop first:** unpack it and start it with \`--demo\` for a made-up household
(\`./hemma-mac-arm64 --demo\`), then open http://localhost:8080 - the login shows in its window.
The programs aren't signed: on a Mac run \`xattr -d com.apple.quarantine hemma-mac-arm64\` once (or
right-click it in Finder, then Open).

**Windows:** no ready-made program yet - build it from the source in a few minutes:
[docs/windows.md]($repo/blob/v$ver/docs/windows.md).

Each download has a \`SHA256SUMS\` inside, and the release's own covers them all - each signed with
the project's key (\`SHA256SUMS.sig\`, Ed25519), which every Hemma has built in: an installed one
checks a download with \`hemma --verify-release <its folder>\`, and updates take nothing else. The
programs say which commit they were built from: \`hemma --version\`. What changed in every version:
[CHANGELOG.md]($repo/blob/v$ver/CHANGELOG.md).
EOF
    # every commit since the previous release (HEMMA_PREV: from another tag or commit instead)
    prev=${HEMMA_PREV:-$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null || true)}
    if [ -n "$prev" ] && [ "$prev" != "v$ver" ]; then
        printf '\n## Commits since %s\n\n' "$prev"
        git log --no-merges --format='- %s (%h)' "$prev..HEAD"
        printf '\nSide by side: %s/compare/%s...v%s\n' "$repo" "$prev" "$ver"
    else
        printf '\nThe first release: the versions before it are in CHANGELOG.md, every commit in\n%s/commits/v%s\n' "$repo" "$ver"
    fi
} > "$notes"
ls -l "$out"
