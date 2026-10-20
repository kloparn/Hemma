#!/bin/sh
# /opt/hemma-image/firstboot.sh — the first start of a Pi from Hemma's ready-made image (issue #71), run
# as root by hemma-firstboot.service. Each step can run again: a run that fails is tried again at the
# next start, and the unit is turned off only after Hemma's installer went through.
#
# 1. Security updates by themselves: unattended-upgrades and what it needs came with the image as .deb
#    files (checked against Debian's signatures when the image was made) - no internet needed here.
# 2. The name "hemma" (http://hemma.local), unless Raspberry Pi Imager gave the machine one.
# 3. Hemma's own installer, as for any install: the hemma user, the hardened service, the update and
#    disk helpers, and - made now, on this Pi - the password secret and the one-time setup code.
set -u
B=/opt/hemma-image
log() { echo "hemma-firstboot: $*"; }

if ! dpkg -s unattended-upgrades >/dev/null 2>&1 && ls "$B"/debs/*.deb >/dev/null 2>&1; then
    echo "unattended-upgrades unattended-upgrades/enable_auto_updates boolean true" | debconf-set-selections
    if DEBIAN_FRONTEND=noninteractive dpkg -i "$B"/debs/*.deb; then
        log "automatic security updates are on"
    else
        log "the security-update packages didn't install - Hemma goes on without them"
    fi
fi
if dpkg -s unattended-upgrades >/dev/null 2>&1 && [ ! -f /etc/apt/apt.conf.d/20auto-upgrades ]; then
    printf 'APT::Periodic::Update-Package-Lists "1";\nAPT::Periodic::Unattended-Upgrade "1";\n' \
        >/etc/apt/apt.conf.d/20auto-upgrades
fi

if [ "$(hostname)" = raspberrypi ]; then
    hostnamectl set-hostname hemma 2>/dev/null || { echo hemma >/etc/hostname && hostname hemma; }
    sed -i 's/\(127\.0\.1\.1[[:space:]]\{1,\}\)raspberrypi\b/\1hemma/' /etc/hosts
    grep -q '^127\.0\.1\.1' /etc/hosts || echo "127.0.1.1	hemma" >>/etc/hosts
    # (cloud-init keeps it from now on instead of naming the machine again)
    printf '# Written by Hemma'"'"'s first start: keep the name it chose.\npreserve_hostname: true\n' \
        >/etc/cloud/cloud.cfg.d/99_hemma.cfg 2>/dev/null || true
    systemctl try-restart avahi-daemon >/dev/null 2>&1 || true
    log "named this machine hemma (http://hemma.local)"
fi

if ! (cd "$B/hemma-pi64" && HEMMA_INSTALL_WAIT=120 sh ./install-pi.sh ./hemma-pi64); then
    log "Hemma's installer didn't finish - trying again at the next start (journalctl -u hemma-firstboot)"
    exit 1
fi
systemctl disable hemma-firstboot.service >/dev/null 2>&1 || true
log "done: open http://$(hostname).local and follow the welcome page"
