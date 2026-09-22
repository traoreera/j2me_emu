# AGENTS.md — J2ME MIDP Emulator

## Build

**Single command (matches CI):**
```
g++ -std=c++17 -O2 -I. -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
    main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
    hal/display.cpp hal/input.cpp hal/png.cpp \
    vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
    vm/natives.cpp vm/midp_natives.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```

**Note:** Three `-I` roots are required. `-I.` is needed because `hal/file.cpp`,
`hal/display.cpp` and `hal/input.cpp` use project-root-relative `#include "hal/file.h"`,
while `hal/jar_reader.cpp`/`hal/inflate.cpp`/`vm/*.cpp` use plain relative includes.
All three `-I.` `-Ihal` `-Ivm` flags must be present together.

**CMake:** `mkdir -p build && cd build && cmake .. && make`
- CMake pre-defines `JAR_READER_INDEX_IN_RAM` (keep for PC dev; drop for RP2040).

## Running

```
./j2me_emu games/assasin.jar    # default
./j2me_emu games/mission.jar
```

**Key env vars (all read in `main.cpp`):**
- `JME_MAXFRAMES=n` — auto-quit after n frames (headless/CI)
- `JME_AUTOKEY=5|0|*|#|FIRE|SOFT1|SOFT2|LEFT|RIGHT|UP|DOWN` — hold a key from frame 0
- `JME_AUTOKEYFRAME=n` — with `JME_AUTOKEY`: send a one-frame tap of that key at frame n (e.g. `JME_AUTOKEY=FIRE JME_AUTOKEYFRAME=285`)
- `JME_DUMP=path.ppm` — dump final framebuffer as PPM on exit
- `JME_WIDTH=n` / `JME_HEIGHT=n` — override emulated resolution (default 240x320)
- `JME_HEAP=n` — heap size in KB (default 512); Assassin's Creed 2 needs `JME_HEAP=4096`
- `SDL_VIDEODRIVER=dummy` — headless mode; combine with `JME_MAXFRAMES` for CI

Note: Assassin's Creed 2 requires **landscape** (`JME_WIDTH=800 JME_HEIGHT=480`); in portrait it refuses with its own message. `System.currentTimeMillis()` uses a **simulated clock** (vm/natives.cpp `virtualMillis`, +16 ms/frame from `midp::tick`), so the intro's ~4.5 s timer fires at ~frame 280 — the game then shows its loading dialog and stalls (loader never completes).

## Architecture pipeline

**JAR/ZIP → class file parser → runtime/class loader → bytecode interpreter → native API bridge → HAL (display/input).**

Key sub-systems (see `CLAUDE.md` for detail):
- `hal/file.cpp` — only `FILE*`/libc dependency; porting target
- `hal/jar_reader.cpp` — ZIP/JAR reader, sequential central directory scan, caller-provided buffers
- `hal/inflate.cpp` — bit-at-a-time DEFLATE decoder, no separate 32 KB window buffer
- `vm/interpreter.cpp` — cooperative instruction budget (`setInstrBudget`), yield callback for fiber suspension
- `vm/natives.cpp` / `vm/midp_natives.cpp` — fiber-based threading (256 KB stack + `ucontext_t` per `Thread.start()`ed `Runnable`)

## Critical constraints

- **No full file in RAM.** All JAR/class I/O goes through `hal::file_*` seek/read, not slurping.
- **No general-purpose heap allocation in the JVM.** `Heap` is a bump allocator (224 KB default, no GC). `malloc`/`new` in `vm/`/`hal/` must stay PC-only debug paths.
- **Caller-provided output buffers** for extraction/decompression — never allocate internally in `jar_reader`/`inflate`.
- **Manifest parser has no RFC 822 line-folding support** — acceptable since real `MIDlet-*` fields fit on one line.
- **No ZIP64 support** — J2ME jars are always < 4 GB.
- **Huffman decode is bit-at-a-time** (no fast lookup table) — intentional RAM/simplicity tradeoff.

## Porting to RP2040

Only `hal/file.cpp` depends on `FILE*`/libc. Replace with `hal_file_*` flash/SD primitives. Drop `JAR_READER_INDEX_IN_RAM` and add `JAR_READER_NO_COMMENT_SCAN` (avoids ~64 KB stack reserve). See `INTEGRATION.md` for the target `CMakeLists.txt` shape.

## Testing

No automated suite. Verify manually: build, run headless against `games/assasin.jar`/`games/mission.jar`, check stdout for `MIDlet: ... classe principale: ...` and clean `Emulation terminee apres N frames`. Use `JME_DUMP` to inspect a rendered frame.