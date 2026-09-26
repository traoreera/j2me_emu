#!/bin/sh
# Démarre la VM (SSH sur localhost:2222, utilisateur pi). Journal série : $PI_EMU_DIR/console.log
D="${PI_EMU_DIR:-$HOME/pi-emu}"; cd "$D" || exit 1
[ -f qemu.pid ] && kill -0 "$(cat qemu.pid)" 2>/dev/null && { echo "déjà lancée"; exit 0; }
exec qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -m 512 \
  -bios /usr/share/edk2/aarch64/QEMU_EFI.fd \
  -drive if=virtio,file=debian.qcow2,format=qcow2 \
  -drive if=virtio,file=seed.iso,format=raw,media=cdrom,readonly=on \
  -drive if=virtio,file=data.raw,format=raw \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:console.log -pidfile qemu.pid -daemonize
