# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

A J2ME/MIDP emulator written in C++17, targeting both PC (SDL2) and, eventually,
an RP2040 microcontroller (264 KB RAM). It reads standard `.jar` MIDlets
(`games/*.jar`), parses their `.class` files, and runs the bytecode on a
custom JVM interpreter with a hand-written MIDP/CLDC native API layer (lcdui,
MIDlet lifecycle, etc.) rendering into an RGB565 framebuffer.

Everything is written with the RP2040 port in mind: no dynamic allocation in
hot paths, streaming I/O via callbacks, caller-provided buffers. The only
component that depends on `FILE*`/libc is `hal/file.cpp`.

Comments and commit-adjacent docs in this repo are largely in French; code
identifiers are in English.

## Build

Preferred (single command, matches CI expectations):
```bash
g++ -std=c++17 -O2 -I. -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
    main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
    hal/display.cpp hal/input.cpp hal/png.cpp \
    vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
    vm/natives.cpp vm/midp_natives.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```
Note the three include roots: `-I.` is required because `hal/file.cpp`,
`hal/display.cpp` and `hal/input.cpp` `#include "hal/file.h"` (project-root-relative),
while `hal/jar_reader.cpp`/`hal/inflate.cpp`/`vm/*.cpp` use plain relative
includes (`"jar_reader.h"`, `"../hal/jar_reader.h"`) — the include style is
inconsistent across files, so all three `-I` flags are needed together.

Or via CMake (`CMakeLists.txt`, needs the `sdl2` dev package via pkg-config):
```bash
mkdir -p build && cd build && cmake .. && make
```

## Running

```bash
./j2me_emu games/assasin.jar   # default if no arg given
./j2me_emu games/mission.jar
```

Useful env vars (read in `main.cpp`):
- `JME_MAXFRAMES=n` — auto-quit after n frames (useful for headless/CI runs)
- `JME_AUTOKEY=5|0|*|#|FIRE|SOFT1|SOFT2|LEFT|RIGHT|UP|DOWN` — hold a key from frame 0 (for scripted smoke tests)
- `JME_DUMP=path.ppm` — dump the final framebuffer as a PPM image on exit
- `SDL_VIDEODRIVER=dummy` — run headless (no window), combine with the above for CI/agent sanity checks, e.g.:
  ```bash
  SDL_VIDEODRIVER=dummy JME_MAXFRAMES=5 ./j2me_emu games/assasin.jar
  ```

## Testing

No automated test suite exists. Verification is manual: build, then run
against `games/assasin.jar` / `games/mission.jar` headless as above and check
stdout for `MIDlet: ... classe principale: ...` and a clean `Emulation
terminee apres N frames`, or use `JME_DUMP` to inspect a rendered frame.

## Architecture

The pipeline is: **JAR/ZIP → class file parser → runtime/class loader →
bytecode interpreter → native API bridge → HAL (display/input)**.

### `hal/` — hardware abstraction layer (PC today, RP2040 later)
- `file.*` — `FILE*`-backed file I/O on PC; the single file to replace when
  porting to RP2040 (flash/SD primitives).
- `jar_reader.*` — ZIP/JAR reader. Locates the EOCD, scans the central
  directory (sequential scan by default — no entry list kept in RAM), extracts
  entries into caller-provided buffers. `JAR_READER_INDEX_IN_RAM` builds a RAM
  index for fast repeated PC-side lookups; drop it for the RP2040 target.
  `JAR_READER_NO_COMMENT_SCAN` disables a ZIP-comment fallback scan that can
  reserve ~64 KB of stack.
- `inflate.*` — self-contained DEFLATE (RFC 1951) decoder (bit reader +
  canonical Huffman decode), written from scratch instead of depending on
  zlib/miniz. Decompresses directly into the caller's destination buffer
  (which doubles as the sliding window — no separate 32 KB window buffer).
- `png.*` — minimal PNG decoder for MIDlet image resources.
- `display.*` — SDL2-backed RGB565 framebuffer (240x320) + a 5x7 bitmap font
  renderer (`display_draw_text`). On RP2040 this becomes the real screen driver.
- `input.*` — SDL2 key mapping to a J2ME key bitmask (D-pad, two softkeys, 0-9, `*`, `#`).

### `vm/` — the JVM subset
- `class_file.*` — `.class` file parser: constant pool, fields, methods, the
  `Code` attribute (bytecode, exception handlers), descriptor parsing
  (`FieldDesc`/`MethodDesc`).
- `runtime.h/.cpp` — `Value` (tagged 64-bit union: int/float/ref/long/double),
  `Obj`/`ObjKind` (heap objects: instances, arrays, strings), `ClassInfo`
  (linked class metadata: super, interfaces, methods, fields, statics),
  `Heap` (a flat bump allocator, default 224 KB pool, no GC — `reset()` is the
  only reclamation), and `Runtime` (class loading/resolution, both from the
  JAR via `loadFromJar` and synthetic native classes via
  `registerNativeClass`/`lookupNativeClass`).
- `interpreter.h/.cpp` — the bytecode interpreter (`execBytecode`) plus
  `invoke`/`invokeStatic`/`invokeSpecial`/`invokeVirtual` entry points and
  `ensureInit` (`<clinit>` running). Frames are allocated from a ring-bump
  arena (`frameAlloc`/`frameFree`), not `new`. Supports a **cooperative
  instruction budget** (`setInstrBudget`/`instrBudgetLeft`): when the budget
  hits 0 mid-method, `execBytecode` bails out (`okResult=false`) so control
  returns to the frame loop; this is how `java.lang.Thread.start()` is
  emulated (see below) without real OS/green threads.
- `native.h` / `natives.cpp` — the native-method registry (string key
  `"Class.method:desc"` → `std::function<void(NativeContext*)>`) plus CLDC
  core natives (`java.lang.*`, threading). `jme_threadStart`/`jme_threads`/
  `jme_threadForget` in `natives.cpp` implement Thread scheduling: each
  "thread" is really just a `Runnable` re-invoked with a small instruction
  budget (4000 instrs, see `midp_natives.cpp`) once per tick until it
  finishes — a cooperative scheduler, not preemptive.
- `midp.h` / `midp_natives.cpp` — the MIDP/CLDC API surface: MIDlet
  lifecycle, `javax.microedition.lcdui` (`Display`, `Canvas`, `GameCanvas`,
  `Graphics`, `Font`, `Image`). `midp::init()` registers these as native
  classes on a `Runtime`; `midp::tick()` (driven from `main.cpp`'s loop) is
  the per-frame pump: dispatches real key events, advances scheduled
  threads, repaints if requested, and presents the frame. `Graphics` draws
  either into an off-heap ARGB `Image` buffer or into the shared RGB565
  `GameCanvas` buffer (`g_canvas565`, double-buffered) that `flushGraphics()`
  presents to the HAL.

### `main.cpp` — orchestration
Opens the JAR → reads `META-INF/MANIFEST.MF` → inits `hal::display`/`hal::input`
→ registers natives (`initNatives()`, `midp::init()`) → loads and instantiates
the MIDlet's main class → calls `<init>` then `startApp()` → runs the frame
loop (`input_poll` → `midp::tick` → `display_present`) until a softkey exit,
`notifyDestroyed()`, or `JME_MAXFRAMES` is hit.

## Key design constraints to preserve

- **No full file in RAM.** All JAR/class I/O goes through `hal::file_*`
  seek/read, not slurping. When adding features, keep this pattern.
- **No general-purpose heap allocation in the JVM.** `Heap` is a bump
  allocator with no free/GC; `malloc`/`new` in `vm/` and `hal/` should stay
  reserved for PC-only debug paths (e.g. the one-off `malloc` in `main.cpp`'s
  PNG smoke test), not the interpreter hot path.
- **Caller-provided output buffers** for extraction/decompression — never
  allocate internally in `jar_reader`/`inflate`.
- **Manifest parser has no RFC 822 line-folding support** — acceptable since
  real-world `MIDlet-*` fields fit on one line.
- No ZIP64 support (unneeded — J2ME jars are always < 4 GB).
- Huffman decode in `inflate.cpp` is bit-at-a-time (no fast lookup table) —
  intentional RAM/simplicity tradeoff; if jar loading is too slow on RP2040,
  this is the known optimization target.

## Porting to RP2040

Only `hal/file.cpp` depends on `FILE*`/libc; every other HAL/VM file is
hardware-agnostic. Replace it with `hal_file_*` flash/SD primitives, drop
`JAR_READER_INDEX_IN_RAM`, and add `JAR_READER_NO_COMMENT_SCAN` (see
`INTEGRATION.md`, in French, for the target `CMakeLists.txt` shape).
