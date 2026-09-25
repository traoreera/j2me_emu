#!/bin/sh
# Prépare une VM Debian 12 arm64 (même base que Raspberry Pi OS Lite 64 bits, "bookworm")
# émulée par QEMU avec le profil Pi Zero 2 W : Cortex-A53 x4, 512 Mo de RAM.
# Tout vit dans $PI_EMU_DIR (défaut ~/pi-emu), hors du dépôt.
# NB : émulation TCG (x86 -> arm64) => les TEMPS ne sont pas représentatifs
# (10 à 30x plus lent) ; la mémoire (RSS), la correction 64 bits et le build le sont.
set -e
D="${PI_EMU_DIR:-$HOME/pi-emu}"
mkdir -p "$D"; cd "$D"
[ -f debian.qcow2 ] || curl -L -o debian.qcow2 \
  https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-genericcloud-arm64.qcow2
[ -f key ] || ssh-keygen -q -t ed25519 -N "" -f key
# qemu-img absent sur cette machine : pas d'overlay/redimensionnement. Le système (debian.qcow2)
# est utilisé tel quel ; le code, le swap et les builds vivent sur un 2e disque brut monté sur /work.
[ -f data.raw ] || truncate -s 8G data.raw
cat > user-data <<EOF
#cloud-config
hostname: pizero2
users:
  - name: pi
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat key.pub)
package_update: true
packages: [build-essential, libsdl2-dev, pkg-config, cmake, rsync]
fs_setup:
  - {label: work, filesystem: ext4, device: /dev/vdc, overwrite: false}
mounts:
  - [ LABEL=work, /work, ext4, "defaults,nofail" ]
runcmd:
  - [ sh, -c, "mkdir -p /work/games && chown pi:pi /work/games && fallocate -l 1G /work/swap && chmod 600 /work/swap && mkswap /work/swap && swapon /work/swap" ]
  - [ touch, /work/.provisioned ]
EOF
echo "instance-id: pizero2-1" > meta-data
rm -f seed.iso
xorriso -as mkisofs -quiet -o seed.iso -V cidata -J -r user-data meta-data
echo "OK : $D prêt. Lancer : tools/pi-emu/run.sh"
