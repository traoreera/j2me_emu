# docs/AGENTS.md — J2ME MIDP Emulator

## Build

**Single command (matches CI):**
```
g++ -std=c++17 -O2 -Isrc -DJAR_READER_INDEX_IN_RAM \
    src/app/main.cpp src/hal/*.cpp src/core/*.cpp src/cldc/*.cpp \
    src/midp/*.cpp src/kernel/kernel.cpp src/kernel/audio/*.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```

**Note:** single include root `-Isrc`; includes are written from it (`"hal/file.h"`, `"core/runtime.h"`). `tools/build.sh` wraps the command.

**CMake:** `mkdir -p build && cd build && cmake .. && make`
- CMake pre-defines `JAR_READER_INDEX_IN_RAM` (keep for PC dev; drop for RP2040).

## Running

```
./j2me_emu games/assasin.jar    # default
./j2me_emu games/mission.jar
```

**Key env vars (all read in `src/app/main.cpp`):**
- `JME_MAXFRAMES=n` — auto-quit after n frames (headless/CI)
- `JME_AUTOKEY=5|0|*|#|FIRE|SOFT1|SOFT2|LEFT|RIGHT|UP|DOWN` — hold a key from frame 0
- `JME_AUTOKEYFRAME=n` — with `JME_AUTOKEY`: send a one-frame tap of that key at frame n (e.g. `JME_AUTOKEY=FIRE JME_AUTOKEYFRAME=285`)
- `JME_AUTOTAPS="f1:KEY,f2:KEY,..."` — list of one-frame taps at given frames (frame first, `:`, then key name)
- `JME_KEYMAP="sdlkey=TOKEN,..."` — remap physical keys on top of the default table; `TOKEN ∈ UP|DOWN|LEFT|RIGHT|FIRE|SOFT1|SOFT2|STAR|HASH|0..9|NONE` (`NONE` unbinds, e.g. `JME_KEYMAP=escape=SOFT2,f2=SOFT1`). Falls back to a per-game file `<jar-without-extension>.keys` next to the `.jar` (one `sdlkey=TOKEN` per line) when the env var is unset.
- `JME_DUMP=path.ppm` — dump final framebuffer as PPM on exit
- `JME_WIDTH=n` / `JME_HEIGHT=n` — override emulated resolution (default 240x320)
- `JME_HEAP=n` — heap size in KB (default 512); Assassin's Creed 2 needs `JME_HEAP=4096`
- `JME_AUDIO=sdl|stub|off` — audio backend; default: `stub` when `SDL_VIDEODRIVER=dummy`, else `sdl`
- `JME_WAVDUMP=path` — debug: dump each Player InputStream raw bytes to `<path>_<slot>.wav`
- `SDL_VIDEODRIVER=dummy` — headless mode; combine with `JME_MAXFRAMES` for CI

**Keyboard (physical, `src/hal/input.cpp`):** arrows = D-pad, Enter/Space = FIRE, F1 = SOFT1 (left softkey), F2 **and** Escape = SOFT2 (right softkey), 0-9 = digits, Shift+8 = `*`, `#` = `#`. **Quit = F12 or Ctrl+Q or window close — NOT the softkeys** (F1/F2/Esc are game keys; a GAMELOFT regression saw SOFT1/SOFT2 presses used as the quit shortcut, silently swallowing next/back for games like mission.jar which maps them to its menu).

Note: Assassin's Creed 2 requires **landscape** (`JME_WIDTH=800 JME_HEIGHT=480`); in portrait it refuses with its own message. `System.currentTimeMillis()` uses a **simulated clock** (src/cldc/natives.cpp `virtualMillis`, +16 ms/frame from `midp::tick`), so the intro's ~4.5 s timer fires at ~frame 280 — the game then shows its loading dialog and stalls (loader never completes).

## Architecture pipeline

**JAR/ZIP → class file parser → runtime/class loader → bytecode interpreter → native API bridge → HAL (display/input).**

Key sub-systems (see `CLAUDE.md` for detail):
- `src/hal/file.cpp` — only `FILE*`/libc dependency; porting target
- `src/hal/jar_reader.cpp` — ZIP/JAR reader, sequential central directory scan, caller-provided buffers
- `src/hal/inflate.cpp` — bit-at-a-time DEFLATE decoder, no separate 32 KB window buffer
- `src/core/interpreter.cpp` — cooperative instruction budget (`setInstrBudget`), yield callback for fiber suspension
- `src/cldc/natives.cpp` / `src/midp/midp_natives.cpp` — fiber-based threading (256 KB stack + `ucontext_t` per `Thread.start()`ed `Runnable`)
- `src/kernel/kernel.cpp` — MCU-portable core: driver registry (`driverRegister`/`kernelBoot`) + monotonic clock (`setMillisProvider`/`millis`); no STL/libc
- `src/kernel/audio/` — kernel audio (mono 16-bit, 22050 Hz, fixed 8 voices, no alloc): `audio.cpp` mixer (PCM + sine tone + tone-seq voices, `playSample`/`playTone`/`playToneSeq`), `sdl_audio.cpp` PC backend (guarded by `__has_include(<SDL2/SDL.h>)`), `stub_audio.cpp` MCU/headless refactor. The VM (`src/midp/midp_natives.cpp`) drains the game's InputStream, parses RIFF/WAV (8/16-bit, mono/stereo, resample→22050) or `audio/x-tone-seq` (ToneControl subset), and drives a kernel voice; `advanceSilent` keeps FIFO timing in headless/stub mode.

## Critical constraints

- **No full file in RAM.** All JAR/class I/O goes through `hal::file_*` seek/read, not slurping.
- **No general-purpose heap allocation in the JVM.** `Heap` is a bump allocator (224 KB default, no GC). `malloc`/`new` in `src/core/`/`src/midp/`/`src/hal/` must stay PC-only debug paths.
- **Caller-provided output buffers** for extraction/decompression — never allocate internally in `jar_reader`/`inflate`.
- **Manifest parser has no RFC 822 line-folding support** — acceptable since real `MIDlet-*` fields fit on one line.
- **No ZIP64 support** — J2ME jars are always < 4 GB.
- **Huffman decode is bit-at-a-time** (no fast lookup table) — intentional RAM/simplicity tradeoff.

## Porting to RP2040

Only `src/hal/file.cpp` depends on `FILE*`/libc. Replace with `hal_file_*` flash/SD primitives. Drop `JAR_READER_INDEX_IN_RAM` and add `JAR_READER_NO_COMMENT_SCAN` (avoids ~64 KB stack reserve). See `docs/INTEGRATION.md` for the target `CMakeLists.txt` shape.

## Testing

No automated suite. Verify manually: build, run headless against `games/assasin.jar`/`games/mission.jar`, check stdout for `MIDlet: ... classe principale: ...` and clean `Emulation terminee apres N frames`. Use `JME_DUMP` to inspect a rendered frame.