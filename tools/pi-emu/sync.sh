#!/bin/sh
# Copie le code (sans games/ ni build) dans la VM : ~/games ; les JAR se copient à part (--jars).
D="${PI_EMU_DIR:-$HOME/pi-emu}"; cd "$(dirname "$0")/../.." || exit 1
SSH="ssh -i $D/key -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
rsync -az --delete --exclude .git --exclude '/build*' --exclude CMakeCache.txt --exclude CMakeFiles --exclude Makefile --exclude cmake_install.cmake --exclude CTestTestfile.cmake --exclude 'j2me_*' --exclude games --exclude '*.ppm' -e "$SSH" ./ pi@localhost:/work/games/
[ "$1" = "--jars" ] && rsync -az -e "$SSH" games/ pi@localhost:/work/games/games/
