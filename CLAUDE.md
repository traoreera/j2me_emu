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
- `JME_AUTOKEYFRAME=n` — with `JME_AUTOKEY`: send a one-frame tap of that key on frame n instead of holding from 0 (to interact once the game has reached a given state).
- `JME_DUMP=path.ppm` — dump the final framebuffer as a PPM image on exit
- `JME_WIDTH=n` / `JME_HEIGHT=n` — override the emulated screen resolution (default 240x320, matching the RP2040 target). Some MIDlets hardcode a `getWidth()`/`getHeight()` check against a specific device resolution (e.g. 800x480 WVGA feature phones) and refuse to render on a mismatch — use these to match the JAR's expected profile for testing.
- `JME_HEAP=n` — override the JVM heap size in KB (default 512, i.e. `Heap::kDefaultPoolSize`). The heap is a bump allocator with no GC (see below); asset-heavy MIDlets can exhaust it during resource loading.
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
  instruction budget** (`setInstrBudget`/`instrBudgetLeft`) as an
  anti-runaway-loop safety net, and a **yield callback** (`setYieldFn`/
  `clearYieldFn`): when the budget hits 0 mid-method, if a yield function is
  set, it's called and execution *genuinely suspends and later resumes*
  exactly where it left off (pc, locals, C++ call stack all intact) — see
  the fiber scheduler below. With no yield function set (e.g. `startApp()`/
  `paint()` called synchronously, not from a scheduled thread), budget
  exhaustion still falls back to abandoning the call (`okResult=false`).
- `native.h` / `natives.cpp` — the native-method registry (string key
  `"Class.method:desc"` → `std::function<void(NativeContext*)>`) plus CLDC
  core natives (`java.lang.*`, threading). **Thread scheduling is
  fiber-based** (`natives.cpp`, `ucontext.h`): each `Thread.start()`ed
  `Runnable` gets its own `JmeFiber` (a 256 KB stack + `ucontext_t`).
  `jme_threadResume()` (called once per tick from `midp::tick()`) swaps into
  the fiber to run/resume it; `Thread.sleep()`/`Thread.yield()` (and, as a
  safety net, the interpreter's instruction budget running out) call
  `jme_yieldNow()`, which `swapcontext`s back to the caller — a *real*
  suspend, not a restart-from-scratch. This is what lets a game's main loop
  (`while (true) { update(); repaint(); Thread.sleep(frameDelay); }`, the
  standard MIDP pattern) advance exactly one iteration per emulator tick
  instead of being replayed from the top of `run()` every frame and never
  progressing past whatever it was doing when the budget first ran out.
  `jme_threadStart`/`jme_threads`/`jme_threadForget` still track which
  Runnables are pending; `jme_threadForget` also frees the fiber.
- `midp.h` / `midp_natives.cpp` — the MIDP/CLDC API surface: MIDlet
  lifecycle, `javax.microedition.lcdui` (`Display`, `Canvas`, `GameCanvas`,
  `Graphics`, `Font`, `Image`). `midp::init()` registers these as native
  classes on a `Runtime`; `midp::tick()` (driven from `main.cpp`'s loop) is
  the per-frame pump: dispatches real key events, advances scheduled
  threads, repaints if requested, and presents the frame. `Graphics` draws
  either into an off-heap ARGB `Image` buffer or into the shared RGB565
  `GameCanvas` buffer (`g_canvas565`, dynamically sized to the configured
  resolution — see `JME_WIDTH`/`JME_HEIGHT` above — allocated once in
  `midp::init()`) that `flushGraphics()` presents to the HAL.
  `GameCanvas` is registered under its real MIDP 2.0 package,
  `javax/microedition/lcdui/game/GameCanvas` (**not**
  `javax/microedition/lcdui/GameCanvas` — a real J2ME MIDlet's bytecode
  references the former; get this wrong and every `GameCanvas` subclass's
  `super()` call fails to resolve). It also declares 2 native fields
  (`__fullscreen`, `__gfx`) matching the `GC_FULLSCREEN`/`GC_GFX` cell
  indices used in `midp_natives.cpp` — GameCanvas's own hidden state needs
  real reserved slots, or those indices silently alias the concrete
  subclass's own first two fields (observed: a cached `Graphics` object got
  overwritten by an unrelated app field, so a later `Graphics.getFont()`
  call resolved against the wrong runtime type). The rest of the MIDP 2.0
  Game API (`Sprite`, `TiledLayer`, `LayerManager`, `Layer`) is **not**
  implemented — MIDlets that use them (common for `GameCanvas`-based games)
  will fail to resolve those classes.

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

## JVM correctness pitfalls (already fixed once — don't reintroduce)

Real commercial/obfuscated JARs exercise corners of the JVM spec that hand
tests miss. Found and fixed while bringing up new test JARs:

- **`registerNative()` is last-writer-wins.** `vm/midp_natives.cpp`'s
  `init()` used to re-register `java/lang/Object.getClass`,
  `System.currentTimeMillis`, all of `Math.*`, `String.length/charAt`, etc.
  as `ui_noop` stubs — since `initNatives()` (real implementations, in
  `natives.cpp`) runs *before* `midp::init()` (see `main.cpp`), this
  silently neutered them for every single MIDlet (`currentTimeMillis()`
  always 0, `arraycopy`/`Math.min/max` all no-ops). No error, no crash —
  just wrong behavior. If you add a `regN`/`registerNative` call, grep for
  the same key first.
- **Field lookup must match on (name, descriptor), not name alone.**
  Obfuscators routinely reuse one field name for several types in the same
  class (`a:I`, `a:[B`, `a:Ljava/io/InputStream;`, `a:Lfoo/Bar;` all on the
  same class). `ClassInfo::findField`/`findFieldRecursive` now take an
  optional descriptor (`vm/runtime.h`); `interpreter.cpp`'s
  getstatic/putstatic/getfield/putfield pass it. Without it, unrelated
  fields silently alias the same storage slot.
- **`getstatic`/`putstatic` must trigger `<clinit>`.** Only `invokestatic`
  used to call `ensureInit()`; a class whose first-ever access is a
  `getstatic` (e.g. reading a static array allocated in `<clinit>`) saw an
  uninitialized field. Fixed in `interpreter.cpp`'s 0xb2/0xb3 case.
- **`invokespecial` resolves statically from the referenced class, walking
  its own super chain — never from the receiver's runtime type.** The old
  code fell back to `runtimeClassOf(thisObj)->findMethodVirtual(...)` when
  the referenced class had no matching method; for a `super()` call to a
  native class with no registered `<init>` (e.g. `Canvas`), this could
  resolve back to the *currently-executing* constructor itself → infinite
  recursion → frame arena exhaustion. Now uses
  `declClass->findMethodVirtual(name, desc)` (a plain static-chain walk),
  matching real invokespecial semantics.
- **`tableswitch`/`lookupswitch` jump offsets are relative to the opcode's
  own address, not to the (post-padding) start of the offset table.** Using
  the wrong base landed execution 2-3 bytes into the jump table itself,
  interpreting its bytes as bytecode.
- **`Object.getClass()` must not dereference `o->cls` unconditionally** —
  it's only populated for `ObjKind::Instance`; `String`/array objects have
  it null by construction (`Heap::newString`/`newArray`). Mirror
  `runtimeClassOf()`'s `ObjKind` switch instead.
- `javax.microedition.lcdui.game.GameCanvas` is under the `game`
  sub-package (not directly under `lcdui`) in the real MIDP 2.0 API — get
  this wrong and every `GameCanvas` subclass's `super()` call fails to
  resolve. It also needs 2 *declared* fields for its own hidden state
  (`GC_FULLSCREEN`/`GC_GFX` cell indices in `midp_natives.cpp`) — leaving
  its field list empty makes those indices alias the concrete subclass's
  own first two fields.
- **Hidden native-class state must be reserved on whichever class the
  accessor is actually registered against — not just on the leaf class
  that happens to declare the fields.** `GC_FULLSCREEN`/`GC_GFX` (cell
  indices 0/1, `midp_natives.cpp`) are read/written by `gc_setFullScreen`/
  `gc_getGraphics`/`gc_flushGraphics`/`gc_getKeyStates`, and those same
  functions are `regN`'d for **both** `Canvas` and `GameCanvas` (a plain
  `Canvas`/`FullCanvas` game never touches `GameCanvas` at all). The two
  fields used to be declared only on `GameCanvas`; any MIDlet whose Canvas
  subclass called `setFullScreenMode()`/`getGraphics()` without going
  through `GameCanvas` had those calls silently overwrite cells[0]/cells[1]
  of the *subclass's own* first declared field(s) — no crash at the call
  site, just a corrupted field read back much later (observed on
  `games/prince.jar`: `setFullScreenMode(true)` overwrote the subclass's
  own `MIDlet` back-reference field with the boolean `1`, producing a
  `thisObj=0x1` SIGSEGV many frames afterward, deep inside an unrelated
  `getAppProperty()` call — the kind of bug that's easy to mis-diagnose as
  heap corruption since the write and the crash are far apart in time and
  code). Fixed by declaring `__fullscreen`/`__gfx` on `Canvas` itself (the
  common ancestor both sets of accessors are registered against) instead of
  redundantly on `GameCanvas`. General rule: whenever a native accessor
  hardcodes a cell index, that index must be reserved on every class the
  accessor is `regN`'d for, via `registerNativeClass`'s field list on their
  nearest common ancestor — not just on whichever one happens to be
  imagined as "the real owner".
- `com.nokia.mid.ui.FullCanvas` (Nokia UI API extension, not standard MIDP)
  shows up in real Nokia-targeted games in place of `Canvas`/`GameCanvas`.
  Registered as a native class extending `Canvas` (same API surface); its
  `static final int` key constants are compile-time-inlined by javac, so no
  field values need to be provided.
- **A native method needs both a `registerNative()` entry *and* a matching
  entry in its class's `regClass()` method-signature list.** `invokeVirtual`
  resolves the call by walking `ClassInfo::methods` first
  (`findMethodVirtual`); only once that lookup succeeds does dispatch fall
  through to the native registry by string key. Adding only the
  `registerNative()` call (e.g. `String.indexOf:(II)I`) still fails with
  "méthode introuvable" — the signature must also appear in the `regClass`
  call for that class. Found while getting `games/prince.jar` running:
  `String.indexOf(int, int)` and `Graphics.drawString(String,int,int,int)`
  (the real 4-arg MIDP signature, with anchor — the 3-arg version some code
  here registered isn't a real MIDP overload) were both missing from their
  class's declared method list even though natives existed for them.

## Porting to RP2040

Only `hal/file.cpp` depends on `FILE*`/libc; every other HAL/VM file is
hardware-agnostic. Replace it with `hal_file_*` flash/SD primitives, drop
`JAR_READER_INDEX_IN_RAM`, and add `JAR_READER_NO_COMMENT_SCAN` (see
`INTEGRATION.md`, in French, for the target `CMakeLists.txt` shape).
