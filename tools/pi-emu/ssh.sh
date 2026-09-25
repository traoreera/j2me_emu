#!/bin/sh
# ssh vers la VM : tools/pi-emu/ssh.sh [commande...]
D="${PI_EMU_DIR:-$HOME/pi-emu}"
exec ssh -i "$D/key" -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR pi@localhost "$@"
