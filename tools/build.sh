#!/bin/sh
# Build direct (sans CMake) : tools/build.sh [sortie]  (défaut : ./j2me_emu)
cd "$(dirname "$0")/.." || exit 1
g++ -std=c++17 -O2 -Isrc -DJAR_READER_INDEX_IN_RAM \
  src/app/main.cpp src/hal/*.cpp src/core/*.cpp src/cldc/*.cpp \
  src/midp/*.cpp src/kernel/kernel.cpp src/kernel/audio/*.cpp \
  -o "${1:-j2me_emu}" $(pkg-config --cflags --libs sdl2)
