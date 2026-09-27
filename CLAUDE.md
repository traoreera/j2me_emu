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
component that depends on `FILE*`/libc is `src/hal/file.cpp`.

Comments and commit-adjacent docs in this repo are largely in French; code
identifiers are in English.

## Build

Preferred (single command, matches CI expectations):
```bash
g++ -std=c++17 -O2 -Isrc -DJAR_READER_INDEX_IN_RAM \
    src/app/*.cpp src/hal/*.cpp src/core/*.cpp src/cldc/*.cpp \
    src/midp/*.cpp src/kernel/kernel.cpp src/kernel/audio/*.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```
Single include root: `-Isrc`; every include is written from it (`"hal/file.h"`, `"core/runtime.h"`, `"midp/midp_internal.h"`, `"kernel/kernel.h"`). `tools/build.sh [out]` runs the command above.

Or via CMake (`CMakeLists.txt`, needs the `sdl2` dev package via pkg-config):
```bash
mkdir -p build && cmake -S . -B build && cmake --build build
```
Use the explicit `-S`/`-B` form, not `cd build && cmake ..` — in at least one
sandboxed shell environment the plain positional form reliably misresolved
its binary directory and silently regenerated build files in the *source*
root instead of `build/` (reproduced repeatedly, even from a from-scratch
`build/`), which then makes `build/`'s own Makefile look permanently stale
(e.g. missing a target that was just added to `CMakeLists.txt`) even though
CMake reports success. `-S`/`-B` sidesteps whatever that resolution issue
is. If you ever see `cmake ..` claim "Build files have been written to:
<source dir>" instead of `<source dir>/build`, that's this happening —
delete any stray `Makefile`/`CMakeCache.txt`/`CMakeFiles/` that ended up in
the source root and reconfigure with `-S`/`-B`.

## Running

```bash
./j2me_emu                     # launcher (menu des jeux de games/)
./j2me_emu games/assasin.jar   # lance directement un jeu
./j2me_emu games/mission.jar
```

Useful env vars (read in `src/app/main.cpp`):
- `JME_MAXFRAMES=n` — auto-quit after n frames (useful for headless/CI runs)
- `JME_AUTOKEY=5|0|*|#|FIRE|SOFT1|SOFT2|LEFT|RIGHT|UP|DOWN` — hold a key from frame 0 (for scripted smoke tests)
- `JME_AUTOKEYFRAME=n` — with `JME_AUTOKEY`: send a one-frame tap of that key on frame n instead of holding from 0 (to interact once the game has reached a given state).
- `JME_DUMP=path.ppm` — dump the final framebuffer as a PPM image on exit
- `JME_WIDTH=n` / `JME_HEIGHT=n` — override the emulated screen resolution (default 240x320, matching the RP2040 target). Some MIDlets hardcode a `getWidth()`/`getHeight()` check against a specific device resolution (e.g. 800x480 WVGA feature phones) and refuse to render on a mismatch — use these to match the JAR's expected profile for testing. Example: `games/jump.jar` requires `width∈[150,250]` and `height∈[170,250]` (throws and falls back to an error `Alert` outside that range) — run it with `JME_WIDTH=176 JME_HEIGHT=220`. Some Gameloft titles instead require **landscape** (`width > height`) and print a static, non-interactive "please switch to landscape mode" screen at the default portrait 240x320 — not a bug, no key does anything on that screen because it isn't a menu. Example: `games/assasin.jar` (Assassin's Creed 2) needs `JME_WIDTH=800 JME_HEIGHT=480` to reach real, navigable UI. `games/gangstar_rio_city_o_260851.jar` is a **480x800 portrait** build (`JME_WIDTH=480 JME_HEIGHT=800`): at other sizes it still runs but lays its Gameloft splash out for a 480-wide screen (three image tiles at absolute coordinates, so the logo appears cropped in a corner at 240x320). It runs its whole loading sequence and shows the Gameloft logo, then loads ~140 resources (`a.c(I)[B` level unpack, ~100 s of CPU at a fixed 100 M-instruction budget) and reaches its main-menu state (state 3: `paint()` -> `s(2)` -> `aA()` touch hit-tests on the menu rects) — but from there `paint()` issues **no draw call at all**, so the screen stays on the last logo frame. **Now works** (since `Canvas.showNotify()` is called — the game only draws once its canvas has been notified as shown): "Do you want sound?" → main menu (Instant Play / New Game / Options), reached after ~2500 frames at `JME_FRAME_TIME=33`.
- `JME_RMS=0` / `JME_RMSDIR=path` — `RecordStore` est **persistant** par défaut : un fichier par magasin dans `<jeu>.rms/` à côté du `.jar` (format : `u32 nbRecords`, puis `u32 taille + octets` par enregistrement, little-endian ; réécrit à chaque `addRecord`/`setRecord`, supprimé par `deleteRecordStore`). `JME_RMS=0` reste purement en mémoire (runs reproductibles) ; `JME_RMSDIR` change le dossier. Un fichier corrompu/tronqué est ignoré (magasin vide), jamais fatal.
- `JME_FRAME_TIME=ms` — durée **virtuelle** d'une trame en millisecondes (défaut 16), pas de temps réel. Une valeur ≥ 1000 est convertie de µs en ms avec un avertissement : `JME_FRAME_TIME=33333` avait pour effet de faire avancer l'horloge du jeu de 33 *secondes* par trame, ce qui figeait le dialogue "Sound Set" d'Assassin's Creed 2 (touches et clics ignorés — les minuteries du jeu débordaient).
- `JME_TRACEM=Classe.methode` (ou `*`) — trace générique des appels d'une méthode bytecode : arguments (8 premiers) et valeur retournée. `JME_AUTOTOUCH=x,y` + `JME_AUTOTOUCHFRAME=n` — clic simulé (appui à n, relâchement à n+3).
- `JME_INSTR_BUDGET=n` — budget d'instructions FIXE par thread et par trame (défaut : **adaptatif**, cf. « Thread budget » dans les pièges de performance). À fixer (ex. 200000) pour des runs déterministes / comparaisons pixel-à-pixel entre deux builds.
- `JME_HEAP=n` — override the JVM heap size in KB (default 512, i.e. `Heap::kDefaultPoolSize`). The heap is a bump allocator with no GC (see below); asset-heavy MIDlets can exhaust it during resource loading.
- `SDL_VIDEODRIVER=dummy` — run headless (no window), combine with the above for CI/agent sanity checks, e.g.:
  ```bash
  SDL_VIDEODRIVER=dummy JME_MAXFRAMES=5 ./j2me_emu games/assasin.jar
  ```

## Testing

### Unit tests (`tests/`)

A header-only micro test framework (`tests/framework.h` — `TEST(name)` /
`ASSERT_EQ`/`ASSERT_TRUE`/`ASSERT_FALSE`/`ASSERT_NE`, no gtest/catch2
dependency, in keeping with the rest of the project's "no heavy deps"
stance) covers the pure/logic layers: `src/hal/inflate.*` (DEFLATE, all 3 block
types), `src/hal/png.*` (non-interlaced + Adam7, against fixture PNGs with known
pixel values), `src/hal/jar_reader.*` (`parseManifestText` plus a full on-disk
ZIP round-trip built by hand — stored and deflate entries), `src/core/class_file.*`
(descriptor parsing, `ConstantPool` accessors, a full `ClassFile::parse()`
round-trip on a hand-built minimal `.class`), `src/core/runtime.*` (`Heap`
alloc/reset/auto-grow, `ClassInfo::findField` descriptor disambiguation,
`findMethodVirtual` superclass-chain walk), and `src/core/interpreter.*` (basic
arithmetic, plus regression tests reproducing the exact `tableswitch`/
`lookupswitch` offset bug and the `athrow`/try-catch stack-unwinding bug
documented below, by hand-assembling the bytecode and exception tables).
Deliberately **not** linked against SDL2/`src/kernel/` — it only needs the
logic-layer `.cpp` files, so it builds even without the `sdl2` dev package.

Build and run:
```bash
mkdir -p build && cmake -S . -B build && cmake --build build --target j2me_tests && (cd build && ctest --output-on-failure)
```
(see the note under Build above on why `-S`/`-B` is used instead of
`cd build && cmake ..`), or directly with g++ (no SDL2 needed):
```bash
g++ -std=c++17 -O0 -g -Isrc \
    tests/test_main.cpp tests/test_inflate.cpp tests/test_png.cpp \
    tests/test_class_file.cpp tests/test_runtime.cpp tests/test_interpreter.cpp \
    tests/test_jar_reader.cpp \
    src/hal/inflate.cpp src/hal/jar_reader.cpp src/hal/file.cpp src/hal/png.cpp \
    src/core/class_file.cpp src/core/runtime.cpp src/core/interpreter.cpp src/cldc/natives.cpp \
    -o j2me_tests && ./j2me_tests
```
The binary prints `[PASS]`/`[FAIL]` per test and exits non-zero on any
failure (CI-friendly). Not covered: `src/midp/midp_natives.cpp` (the MIDP API
surface — its natives live in an anonymous namespace, and exercising it
meaningfully needs a wired-up `Runtime`/`Display`/HAL, closer to an
integration test than a unit test) and the fiber-based thread scheduler in
`src/cldc/natives.cpp` (`ucontext.h`-based, stateful and timing-sensitive).

### Manual / integration verification

For anything touching `src/midp/midp_natives.cpp`, the fiber scheduler, rendering,
or overall game compatibility, verification is still manual: build, then run
against `games/assasin.jar` / `games/mission.jar` headless as above and check
stdout for `MIDlet: ... classe principale: ...` and a clean `Emulation
terminee apres N frames`, or use `JME_DUMP` to inspect a rendered frame.

## Architecture

The pipeline is: **JAR/ZIP → class file parser → runtime/class loader →
bytecode interpreter → native API bridge → HAL (display/input)**.

### `src/hal/` — hardware abstraction layer (PC today, RP2040 later)
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
- `png.*` — minimal PNG decoder for MIDlet image resources. 8-bit depth
  only; color types 0/2/3/4/6; both non-interlaced and Adam7-interlaced
  (`hdr.interlace`). Adam7 decodes each of the 7 passes as an independent
  sub-image (own per-scanline filtering, restarting from no previous row at
  the top of each pass) and scatters its pixels into the final buffer at
  `(sx + col*dx, sy + row*dy)` per pass — see `kAdam7` in `png.cpp`. Passes
  with zero columns/rows for a given image size (common for small icons)
  are skipped entirely, contributing no bytes to the decompressed stream.
- `display.*` — SDL2-backed RGB565 framebuffer (240x320) + a 5x7 bitmap font
  renderer (`display_draw_text`). On RP2040 this becomes the real screen driver.
- `input.*` — SDL2 key mapping to a J2ME key bitmask (D-pad, two softkeys, 0-9, `*`, `#`).

### `src/core/`, `src/cldc/`, `src/midp/` — the JVM subset

Layout: `src/app/` (main), `src/core/` (class_file, runtime, interpreter, native.h, debug.h), `src/cldc/` (natives.cpp: java.lang, threads, RecordStore), `src/midp/` (midp_*.cpp), `src/hal/`, `src/kernel/` (kernel + audio), `tests/`, `tools/`, `profiles/`, `cmake/`, `docs/`.
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
- `midp.h` / `midp_*.cpp` — the MIDP/CLDC API surface, split by domain
  (was a single 5 500-line `midp_natives.cpp`): `midp_natives.cpp` (public
  API: `init()` registering the native *classes*, `tick()`, key mapping),
  `midp_core.cpp` (shared globals + `argInt`/`setRef`… helpers,
  `regClass`/`regN`), `midp_graphics.cpp` (`Graphics`/`Font`/`Image`/
  `Canvas`/`GameCanvas`/DirectGraphics, RGB565 rendering), `midp_game.cpp`
  (`Layer`/`Sprite`/`TiledLayer`/`LayerManager`), `midp_ui.cpp` (`Display`/
  `MIDlet`/`Canvas` key natives, `List`/`Form`/`Command`), `midp_io.cpp`
  (`java.io` streams, JAR resources, `java.util.Timer`), `midp_media.cpp`
  (MMAPI: WAV/MIDI/ToneSeq engine). Shared state and the cross-module
  function/type declarations live in `midp_internal.h` (namespace
  `jvm::midp::detail`); everything not used across modules is `static` in
  its own file. Each module exposes `register<Module>Natives()` holding its
  own `regN(...)` lines, called from the start of `init()`; `regClass(...)`
  (class + method-signature declarations) stays centralised in `init()`.
  When adding a native: put the function in the matching module, add its
  `regN` to that module's registrar, and its signature to the class's
  `regClass` list in `init()` — a function used from another module must
  also be declared in `midp_internal.h`. `midp::init()` registers these as
  native classes on a `Runtime`; `midp::tick()` (driven from `src/app/main.cpp`'s
  loop) is the per-frame pump: dispatches real key events, advances
  scheduled threads, repaints if requested, and presents the frame.
  `Graphics` draws either into an off-heap ARGB `Image` buffer or into the
  shared RGB565 `GameCanvas` buffer (`g_canvas565`, dynamically sized to the
  configured resolution — see `JME_WIDTH`/`JME_HEIGHT` above — allocated
  once in `midp::init()`) that `flushGraphics()` presents to the HAL.
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
  Game API (`Sprite`, `TiledLayer`, `LayerManager`, `Layer`) **is now
  implemented** (`midp_game.cpp`, `spr_*`/`tl_*`/`lm_*`/`lay_*`
  functions) — sprite frame/transform/collision, tiled-layer cells +
  animated tiles, and the full `LayerManager` (`append`/`insert`/`remove`/
  `getSize`/`getLayerAt`/`setViewWindow`/`paint`). Index 0 is the **top**
  layer (MIDP): `lm_paint` draws from the last index down to 0 — it used to
  draw 0..n, i.e. an inverted z-order.

### `src/app/main.cpp` — orchestration
Opens the JAR → reads `META-INF/MANIFEST.MF` → inits `hal::display`/`hal::input`
→ registers natives (`initNatives()`, `midp::init()`) → loads and instantiates
the MIDlet's main class → calls `<init>` then `startApp()` → runs the frame
loop (`input_poll` → `midp::tick` → `display_present`) until a softkey exit,
`notifyDestroyed()`, or `JME_MAXFRAMES` is hit.

## Key design constraints to preserve

- **No full file in RAM.** All JAR/class I/O goes through `hal::file_*`
  seek/read, not slurping. When adding features, keep this pattern.
- **No general-purpose heap allocation in the JVM.** `Heap` is a bump
  allocator with no free/GC; `malloc`/`new` in `src/core/`, `src/midp/` and `src/hal/` should stay
  reserved for PC-only debug paths (e.g. the one-off `malloc` in `src/app/main.cpp`'s
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

- **`registerNative()` is last-writer-wins.** `src/midp/midp_natives.cpp`'s
  `init()` used to re-register `java/lang/Object.getClass`,
  `System.currentTimeMillis`, all of `Math.*`, `String.length/charAt`, etc.
  as `ui_noop` stubs — since `initNatives()` (real implementations, in
  `natives.cpp`) runs *before* `midp::init()` (see `src/app/main.cpp`), this
  silently neutered them for every single MIDlet (`currentTimeMillis()`
  always 0, `arraycopy`/`Math.min/max` all no-ops). No error, no crash —
  just wrong behavior. If you add a `regN`/`registerNative` call, grep for
  the same key first.
- **Field lookup must match on (name, descriptor), not name alone.**
  Obfuscators routinely reuse one field name for several types in the same
  class (`a:I`, `a:[B`, `a:Ljava/io/InputStream;`, `a:Lfoo/Bar;` all on the
  same class). `ClassInfo::findField`/`findFieldRecursive` now take an
  optional descriptor (`src/core/runtime.h`); `interpreter.cpp`'s
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
- **`lookupswitch`'s pair table starts at `base + 8`, not `base + 12`.**
  `tableswitch`'s header is 3 fields (`default`/`lo`/`hi`, 4 bytes each =
  12) before its offset table, but `lookupswitch`'s header is only 2 fields
  (`default`/`npairs` = 8 bytes) before its `(match, offset)` pairs — the
  `+12` was copy-pasted from `tableswitch` without adjusting for the
  different header size. This misaligned every pair by 4 bytes: entry `i`'s
  *offset* field was read as if it were entry `i`'s *match value*, and entry
  `i+1`'s real match value was read as entry `i`'s offset. The real match
  value was therefore (almost) never compared against, so `key` essentially
  never matched anything and the `default` branch fired unconditionally —
  no error, no crash, just every `lookupswitch` silently behaving as if it
  had zero cases. Games whose `paint()`/state-machine dispatch is driven by
  a `switch` on a state field compiled to `lookupswitch` (multi-case,
  non-contiguous values — very common for obfuscated state machines) got
  stuck permanently on their initial state, painting nothing, with the
  emulator otherwise running cleanly to completion (found on
  `games/mortal_combat_new_b_240x320_173007.jar`: a 23-pair state dispatch
  in `paint()` never advanced past its initial "not yet started" state,
  producing a persistent black screen with zero errors in 800+ frames).
  `tableswitch` was never affected — its header genuinely is 12 bytes.
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
- **`GameCanvas` subclasses can still legally override `paint()`, and the
  AMS must still call it via the normal `repaint()`/`paint()` mechanism —
  `getKeyStates()`/`flushGraphics()` are *additional* APIs GameCanvas
  offers, not a mandatory replacement for the standard Canvas event/paint
  model.** `midp::tick()` used to unconditionally skip both the
  `keyPressed`/`keyReleased` dispatch AND the `paint()` callback whenever
  the current Displayable was a `GameCanvas` subclass (`&& !isGameCanvas`
  on both blocks), on the assumption that every GameCanvas game exclusively
  polls `getKeyStates()` and draws via `getGraphics()`/`flushGraphics()`.
  That assumption breaks for any GameCanvas game that still overrides
  `paint()` (a valid, fairly common pattern — e.g. simpler ports that only
  adopted GameCanvas for its extra API surface but kept a Canvas-style
  render flow) — `paint()` would then just never fire, producing a
  perpetually black screen with a render thread ticking every frame and
  zero errors (found on `games/mortalkomb_9moadjwj.jar`: its `cnv extends
  GameCanvas` overrides `paint(Graphics)` — confirmed via `javap` — and its
  render thread calls `repaint()` every loop iteration when its internal
  `sc_repaint` flag is set, but the callback was categorically suppressed).
  Fixed by dropping `!isGameCanvas` from both conditions in `tick()`
  (`src/midp/midp_natives.cpp`) — GameCanvas no longer opts out of either path.
- **The `Graphics` object passed to `paint()` must have its translation and
  clip reset to defaults (origin, full-canvas) before *every* call — this
  is a hard MIDP guarantee, not implementation-defined.** `screenGraphics()`
  (`src/midp/midp_natives.cpp`) returns a singleton, persistent `Obj*`
  (`g_screenGfx`) reused across every `paint()` invocation; it only refreshed
  `CLIPW`/`CLIPH` on each call, leaving `TX`/`TY` (translate) and
  `CLIPX`/`CLIPY` (clip origin) to silently carry over from whatever the
  *previous* `paint()` call left them at. A game that calls
  `g.translate(dx, 0)` mid-frame and doesn't perfectly symmetric-untranslate
  on every code path (e.g. an early `return` on some state branch) sees its
  camera/scroll offset drift further with every single `paint()` call
  instead of resetting to (0,0) each frame — visually: the same background
  sprite redrawn dozens of times at accumulating offsets, plus stale content
  from an earlier screen staying visible wherever the (also drifting) clip
  rect no longer covers. This bug is old but was invisible until the
  `GameCanvas`-`paint()` fix above gave it a MIDlet exercising it for the
  first time. Fixed by having `screenGraphics()` reset `TX`/`TY`/`CLIPX`/
  `CLIPY`/`CLIPW`/`CLIPH` to `(0, 0, 0, 0, screenW(), screenH())` on every
  call, not just clip width/height. Color/font are deliberately left alone
  — MIDP does not guarantee those are reset between `paint()` calls.
- **`Graphics.drawImage`'s BOTTOM anchor (`0x20`) was checked against the
  wrong bit** — `g_drawImage` (`src/midp/midp_natives.cpp`) computed the vertical
  anchor offset (`oy`) with `else if (anchor & 0x08) oy = ih;`, but `0x08`
  is `RIGHT` (a *horizontal* flag) — `BOTTOM` is `0x20`. Any
  `drawImage(img, x, y, HCENTER|BOTTOM)` call — the standard idiom for a
  full-screen background image anchored so its bottom edge sits at `y`
  (`anchor=33=0x21=HCENTER|BOTTOM`) — silently fell through with `oy=0`
  (TOP behavior instead), drawing the image's top-left corner at `y`
  instead of its bottom-left. For a background image roughly
  screen-sized drawn at `y=screenHeight`, that puts the *entire image*
  below the visible screen — no error, no crash, just an apparently
  missing background behind whatever else got drawn on top. Found on
  `games/mortalkomb_9moadjwj.jar`: its menu/battle screens looked
  completely black behind the menu text and HUD despite `createImage`
  succeeding, until the missing bottom-anchored background was traced
  down to this one wrong bit. `g_drawRegion` (the very next function in
  the same file) already had the correct `& 0x20` check — this was an
  isolated copy-paste slip in `g_drawImage` specifically, not a systemic
  misunderstanding of the anchor bits. Also fixed while here: both
  `g_drawImage` and `g_drawRegion` clamped their target coordinates to a
  hardcoded `800x480` "so the image stays at least partially visible" —
  wrong for any other configured resolution (the project's default is
  240x320); now clamped against the actual `screenW()`/`screenH()`.
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
- **Real `try`/`catch` (exception unwinding) is now implemented — don't
  reintroduce the old "any failure aborts the whole call chain" behavior.**
  `athrow` (0xbf) used to unconditionally abort the current method; the
  per-method exception table (`CodeAttribute::handlers`, already fully
  parsed by `class_file.cpp` but never consulted) was dead data. Real J2ME
  bytecode routinely does `throw new Exception(msg)` for expected,
  recoverable conditions (unsupported screen size, missing optional
  resource, ...) with a `catch` block one or more frames up that falls
  back gracefully — without real unwinding, any such throw silently killed
  the whole call chain (commonly the MIDlet's own `<init>`), even though
  the game's own code had a perfectly good fallback path. Implemented in
  `src/core/interpreter.cpp`: `findExceptionHandler()` searches a method's
  handler table for a range covering the current pc whose `catchType`
  matches the exception's runtime type (walking its superclass chain;
  `catchType==0` = catch-all). `athrow` calls it directly; the shared
  invoke dispatch (0xb6–0xb9) calls it again after any failed call using
  `Interpreter::pendingException_` (a single in-flight-exception slot) to
  thread the exception object up through nested `execBytecode` frames —
  mirroring real JVM stack unwinding one C++/Java frame at a time. Found
  and fixed while getting `games/jump.jar` running: `JumpCanvas.<init>`
  throws `Exception` when the device profile looks unsupported, caught by
  `Jump.<init>`, which falls back to an error `Alert` — previously this
  just meant "class `java/lang/Exception` introuvable" and a hard
  MIDlet-instantiation failure.
- `java.lang.Throwable` and its whole hierarchy
  (`Exception`/`RuntimeException`/`NullPointerException`/
  `ArrayIndexOutOfBoundsException`/`ClassCastException`/
  `IllegalArgumentException`/`NumberFormatException`/etc., plus
  `java/io/IOException`) are registered as native classes, but **only
  `Throwable` itself declares `<init>`/`getMessage`/`toString`/
  `printStackTrace` and the `message` field** — subclasses declare
  nothing and inherit everything through ordinary virtual/`invokespecial`
  resolution (which already walks the superclass chain). Don't redeclare
  these per-subclass "to be safe" — it's unnecessary and, per the
  Canvas/GameCanvas pitfall above, redeclaring the field on a subclass
  would misalign the hidden-state cell index.
- **Some `static final` fields are real object constants, not `int`s —
  javac does NOT inline them, so they need genuine static storage and a
  bootstrap value**, same as `System.out`. `javax.microedition.lcdui.
  AlertType.ALARM/CONFIRMATION/ERROR/INFO/WARNING` are `static final
  AlertType`, not `int`; leaving them as an empty field list meant
  `getstatic AlertType.ERROR` failed outright. Fixed the same way as
  `System.out`: declare the 5 fields on `AlertType`, then after
  `registerNativeClass` runs, allocate one instance per constant and
  write it directly into `atCls->statics[slot]` (`midp_natives.cpp`,
  right after the `System.out` bootstrap). Any other `static final
  <SomeType>` (not `int`/`String` literal) encountered later needs the
  same treatment — check whether it's javac-inlined (primitive/String
  constant, safe to leave as `none`) before assuming an empty field list
  is fine.
- **`Heap::allocObj`'s auto-grow could allocate a new segment SMALLER than
  the object it was growing to fit, corrupting the heap.** The new
  segment's size was computed as `growTo - capTotal_`, where `growTo` is a
  target for the *total* capacity across all segments (`usedTotal_ + need`,
  itself bounded below by doubling). Because `capTotal_` already includes
  space permanently wasted at the tail of earlier segments (a segment is
  never revisited once abandoned), subtracting it does not guarantee the
  *new* segment alone is `>= need` — a single large-enough allocation right
  after a grow (e.g. `sizeof(Obj)` plus a handful of cells on a small heap)
  landed in an undersized segment and wrote past its end, silently
  corrupting glibc's malloc bookkeeping. No crash at the write site; the
  first symptom was an unrelated `malloc`/`sysmalloc` abort much later
  (caught by `tests/test_runtime.cpp`'s `heap_auto_grows_beyond_initial_segment`,
  which allocates repeatedly from a tiny `Heap(64)`). Fixed in
  `src/core/runtime.cpp` by clamping `segSize` to be at least `need` after the
  capacity-doubling computation, not just relying on the total-capacity
  target to imply it.
- **`java.lang.String` was missing `valueOf(int)`, and there was no output
  side of `java.io` at all** (`ByteArrayOutputStream`/`DataOutputStream`/
  `OutputStream` — only the input side existed). Found bringing up
  `games/tigametkch_AXdOxivU.jar` ("Đột Kích", a Vietnamese MIDP-1.0
  MIDlet): it does `String.valueOf(score)` for on-screen text (extremely
  common — missing outright, not just unregistered) and serializes save
  data via the textbook `new DataOutputStream(new
  ByteArrayOutputStream())` idiom before handing the bytes to
  `RecordStore`. `String.valueOf(I)` is now `regN`'d onto
  `java/lang/String` reusing `n_Integer_toStringS` (same static
  `args[0]`-is-the-int convention, same result). The new
  `ByteArrayOutputStream`/`DataOutputStream` natives (`src/midp/midp_natives.cpp`,
  `baos_*`/`dos_*` functions) deliberately mirror the *existing* input-side
  shortcut convention instead of real OOP dispatch: `streamByte`/`streamFill`
  (input) already read `cells[0..2]` of whatever `Obj*` they're given
  assuming an `InputStream`-shaped layout, rather than calling `read()`
  virtually; `dosWriteByte`/`baosStr` do the same thing in reverse
  (`DataOutputStream` pokes its wrapped stream's `cells[0]` directly rather
  than dispatching `write()`). This only works because every `OutputStream`
  in this codebase so far *is* a `ByteArrayOutputStream` — if a future JAR
  wraps something else (unlikely for J2ME, but possible), this would need
  real virtual dispatch instead. `ByteArrayOutputStream`'s accumulated
  bytes are stored the same way `StringBuffer` stores its content: as a
  `String`-kind `Obj` reference in `cells[0]`, replaced wholesale (not
  mutated in place) on every `write()` — reuses `newString()`'s bump
  allocation instead of a separate growable-array scheme, embedded NUL
  bytes included (a `std::string` doesn't care). Also added
  `ByteArrayInputStream.<init>([BII)V` (offset+length constructor) — only
  the full-array `<init>([B)V` existed; this same JAR's `RecordStore`
  read-back path uses the ranged constructor.

## Performance pitfalls

- **`ConstantPool::getUtf8` must return `const std::string&`, not
  `std::string` by value.** It only ever returns a reference into an
  already-stored `CpEntry::utf8`, so a by-value signature forces a full
  string copy on every call — and `getfield`/`putfield`/`invoke*` call it
  (directly or via `getFieldRef`/`getMethodRef`/`getClassName`) on *every
  single bytecode execution*, with no per-call-site caching. Profiled on
  `games/assasin.jar` (gprof, 200 frames, dummy video driver): this one
  signature fixed 15.8M copies and cut raw interpreter CPU time from ~65
  ms/frame to ~14 ms/frame (3.6x) — the difference between ~15 fps
  (visibly juddering, unplayable) and a comfortable 30 fps headroom. If
  profiling ever again shows heavy time in `ConstantPool::get*`/
  `_M_construct`, check this hasn't regressed back to a by-value return.
- **The per-frame loop must not add a fixed `SDL_Delay` on top of
  variable processing time.** `src/app/main.cpp`'s loop used to do
  `SDL_Delay(16)` unconditionally after every frame, regardless of how
  long `midp::tick()` took — guaranteeing a ~62 fps ceiling even when
  processing was instant, and turning any frame-to-frame variance in
  bytecode interpretation time directly into visible stutter (frame time
  = variable processing + fixed 16 ms, so a slow frame plus the extra
  16 ms tax compounds unevenly rather than the engine catching up). Fixed
  with adaptive pacing: measure elapsed time since the frame started,
  sleep only the remainder of a fixed frame budget (`kFrameBudgetMs`,
  currently 33 ms ≈ 30 fps), and skip the sleep entirely if a frame is
  already over budget instead of accumulating lag.
- **`getfield`/`putfield`/`getstatic`/`putstatic` need a resolved-field
  cache keyed by constant-pool index, or field-heavy bytecode is
  unplayable.** Every one of these opcodes used to re-parse the Fieldref's
  `"name:desc"` string (an allocation-heavy `find`+2×`substr`) AND redo a
  linear scan through `ClassInfo::fields`/`findFieldRecursive` (walking the
  superclass chain) on *every single execution* — no different from the
  `getUtf8`-by-value pitfall above, just one level higher. Profiled on
  `games/gangstar_2_kings_of_260766.jar` (gprof, 40 frames, dummy video
  driver — the one MIDlet in `games/` that reads/writes an enormous number
  of instance fields per frame): `ClassInfo::findField` alone was **~50%**
  of total CPU time (4.7M calls in 40 frames), driving the game down to
  ~500 ms/frame (well under 2 fps, and it would time out a 20s headless
  smoke test at only 30 frames in). Fixed by adding
  `ClassInfo::fieldRefCache` (`src/core/runtime.h`) — a `vector<FieldCacheEntry>`
  indexed by constant-pool index, lazily populated by the interpreter (not
  the class loader) on first resolution, storing the resolved
  `MethodRecord*` (and, for `getstatic`/`putstatic` only, the referenced
  class `tc` needed for `ensureInit()`). Cut CPU time on that same
  40-frame run from ~20.6s to ~0.82s (≈25x) — ~20 ms/frame, comfortably
  inside the 33 ms/frame (30 fps) budget. Correctness note: the cache is
  keyed on `(cls, idx)` — the *executing* class and its own constant-pool
  index — not on the receiver's runtime type; this is safe because a
  resolved instance field's slot is a fixed absolute offset shared by every
  subclass that doesn't itself redeclare that field, which is the only
  access pattern this interpreter (and every MIDlet exercised so far)
  relies on. If a future obfuscated JAR is found to *shadow* a field (same
  name+desc redeclared in a subclass) and get miscached results, the cache
  would need to be keyed on the receiver's `ClassInfo*` too, not just `idx`.

- **`getenv()` must never sit in a hot path.** It is a linear scan of
  `environ`. `interpreter.cpp` evaluated ~36 of them, several per `invoke*`
  and per `putstatic`, and `midp` did one per `drawImage`/`Pix` construction.
  Debug flags are now read once (`src/core/debug.h`: `jvm::jmeDebug()`,
  `drawDbg()`, `pixDbg()`; `interpreter.cpp`: `envDebug()`/`envTrace()`).
  The old per-game trace hooks (`JME_QRACE`/`JME_ATRACE`/`JME_FLAGTRACE`/
  `JME_RB`/`JME_WAITDBG`, keyed on the obfuscated class names of specific
  games like `"h"` or `com/nokia/mid/appl/boun/f`) were deleted from the
  interpreter — a game-specific check has no place in the bytecode loop.
- **`Thread.yield()` in a spin-wait burned the whole frame budget.** The
  native only suspended the fiber when the 200k-instruction budget was
  nearly gone, so `while (!cond) Thread.yield();` (a wait for the virtual
  clock or another thread — which can only change *between* frames) ran
  ~30 000 yields and ~34 ms of CPU per frame doing nothing (measured on
  `prince_of_persia_th`). `n_Thread_yield` now detects the pattern: 64
  consecutive `yield()`s separated by < 64 instructions each → suspend for
  the frame. A yield in the middle of real work (loading, decoding) leaves
  far more instructions between calls and is never throttled. Verified
  pixel-identical output at frame 300 on 6 games; CPU/frame dropped
  33.8→21.7 ms (`prince_of_persia_th`), 7.7→2.8 (`gangstar_2`), 5.5→0.5
  (`mission`), 8.7→0.9 (`mortal_combat`).

- **`invoke*` needs the same per-call-site cache as fields.** Every
  `invokestatic/special/virtual/interface` re-parsed `"classe/nom:desc"`
  (`getMethodRef` + `substr`s), looked the class up by name (string hash),
  recomputed `argSlots`/`returnIsVoid`, then resolved the method with
  `ClassInfo::findMethod` — a linear scan comparing name+descriptor strings,
  walking the super chain. Profiled on `games/gangstar_rio` while it
  decodes its level packs (5.2 M invokes): `findMethod` alone was 26 % of CPU.
  `ClassInfo::methodRefCache` (`runtime.h`, indexed by the Methodref's
  constant-pool index, sized once so entry references stay valid across
  nested calls) stores class, name, desc, arg slots, return type; static /
  special targets are resolved once (`staticM`), virtual ones use a
  monomorphic cache (`lastRecv` → `lastM`). Field cache entries also carry
  the slot width (`w`) so `getstatic`/`getfield` no longer rescan the
  descriptor, and `getstatic` skips `ensureInit` once `clinitDone`.
  Pixel-identical output on all 17 games.
- **Thread budget must be adaptive, not 200 000 instructions/frame.**
  `midp::tick()` gave each game thread a fixed 200 000-instruction budget
  (~1–2 ms of CPU) out of a 33 ms frame; a MIDlet that loads/decodes its
  levels in its own thread (Gangstar Rio: LZMA-style range decoding of
  ~370 KB of packed levels, hundreds of millions of instructions) needed
  150+ frames — several seconds of black screen — while 90 % of every frame
  was idle. The budget now adapts: after a frame where the thread consumed
  its whole budget (pure compute), the measured interpreter speed
  (instr/ms, exponential moving average) sets the next budget so threads get
  ~16 ms of CPU per frame (shared among threads; clamp 200 k – 40 M).
  Threads that sleep/yield return long before the budget, so ordinary games
  are unaffected. `JME_INSTR_BUDGET` forces a fixed value for deterministic
  comparisons. Note: results then depend on machine speed by design.

- **First `paint()` is delivered before the game threads run.** On a real
  device `Display.setCurrent()` schedules an immediate paint, long before a
  background loader thread has done anything. `midp::tick()` used to run the
  fibers first, so the first `paint()` arrived after ~200 k instructions of
  loading and could observe half-initialised game state (Gangstar Rio: NPE in
  `paint()` on a not-yet-allocated script array). It now paints first on the
  very first tick (pixel-identical on the other 16 games).

## Porting to RP2040

Only `src/hal/file.cpp` depends on `FILE*`/libc; every other HAL/VM file is
hardware-agnostic. Replace it with `hal_file_*` flash/SD primitives, drop
`JAR_READER_INDEX_IN_RAM`, and add `JAR_READER_NO_COMMENT_SCAN` (see
`docs/INTEGRATION.md`, in French, for the target `CMakeLists.txt` shape).

## Profils par jeu (`<jeu>.conf`) et divers

- `games/<jeu>.conf` (à côté du `.jar`, lu au démarrage par `src/app/main.cpp`) : lignes `CLE=VALEUR`, `#` commentaires. Seules les clés `JME_*` / `SDL_VIDEODRIVER` sont acceptées (`setenv(...,0)` : l'environnement réel gagne). `PROP:Nom=valeur` définit une propriété d'application (`MIDlet.getAppProperty`). Ex. : `assassins_creed_iii_260938.conf` = 480x800 + `PROP:HAS-BLOOD=yes`.
- `getAppProperty` renvoie `""` (pas `null`) pour une clé absente : `null` fait planter AC3 (`"HAS-BLOOD".equals(...)` NPE).
- Les littéraux `ldc` String sont **internés** (`Heap::internString`, aussi `String.intern()`) : les jeux comparent des littéraux avec `if_acmpeq/ne`.
- Souris → `pointerPressed/Released/Dragged` (`src/hal/input`, `midp::pointerEvent`) ; `JME_AUTOTOUCH=x,y` + `JME_AUTOTOUCHFRAME=n`.
- AC III (`assassins_creed_iii_260938.jar`) est un build **thaï uniquement** (`t.eh=15` codé en dur, seule ressource `TH`) : le menu s'affiche mais le texte thaï est illisible (police 5x7 sans glyphes thaï + `String.<init>([CII)V` tronque les chars à 8 bits). Ce n'est pas un bug de sélection de langue.
- `JME_AUTOTOUCHES="x,y,frame;x,y,frame;..."` — plusieurs clics simulés scriptés (appui à `frame`, relâchement à +3), pour traverser les menus tactiles en headless.
- `String.<init>([BIILjava/lang/String;)V` (octets + encodage) manquait : bloquait Assassin's Creed Revelations juste après le splash.
- AC Revelations (`assassins_creed_rev_259065.jar`, 480x800 via `.conf`) est jouable jusqu'à l'intro : "Do you want sound?" → écran titre → menu (Quick Play/New Game/Select Level/High Score) → difficulté → texte d'histoire. Séquence de test : `JME_AUTOTOUCHES="230,400,200;240,400,500;235,400,700;235,400,900;275,390,1100" JME_FRAME_TIME=60 JME_MAXFRAMES=1900` (l'écran est dessiné pivoté de 90°). Gameplay non vérifié.
- `JME_ROTATE=90` — tourne la **vue** (fenêtre) de 90° anti-horaire (`src/hal/display.cpp`, `SDL_RenderCopyEx`), fenêtre 800x480 pour un framebuffer 480x800 ; les clics souris sont re-mappés (`src/hal/input.cpp`). Mis dans les `.conf` des jeux Gameloft 480x800 (Gangstar Rio, AC III, AC Revelations) qui dessinent de côté. Le framebuffer/`JME_DUMP` reste non tourné.

## Cible Raspberry Pi Zero 2 W (Pi OS Lite 64 bits) — `docs/PI_ZERO2_CODE_SPEC.md`

Implémenté (phase 1-2, sans casser le PC ; tests 57/57, 16/17 jeux pixel-identiques, `jump` est non déterministe même contre lui-même) :
- `JME_HEAP_MAX=KiB` — plafond dur de la capacité TOTALE du heap (`Heap(pool, max)`, `maximumCapacity()`) ; au-delà : `outOfMemory()` et alloc `nullptr`, jamais d'écriture hors segment ; plafond < `JME_HEAP` relevé à `JME_HEAP` ; valeur invalide → refus au démarrage. Absent = comportement dev PC (auto-grow illimité).
- `JME_FRAME_BUDGET=ms` — budget RÉEL d'une trame (défaut 33). **Le spec disait `JME_FRAME_TIME` mais celui-ci reste la durée VIRTUELLE de l'horloge du jeu** (les confondre a déjà figé AC2).
- Fenêtre/affichage (`src/hal/display.cpp`) : `JME_WINDOW_WIDTH/HEIGHT`, `JME_FULLSCREEN=1` (desktop plein écran), `JME_SCALE=integer` (facteur entier ≥1, sinon « fit » proportionnel), letterbox noir centré, `JME_VSYNC=0`. Pas de filtrage. Le mapping souris/tactile passe par `display_window_to_logical()` (rotation + letterbox + échelle).
- `JME_RENDER_STATS=1` — en fin de run : trames en retard, heap utilisé/capacité, RSS et pic (`/proc/self/status`), alerte si pic > 256 MiB. (Pas encore de compteurs pixels/cache.)
- `static_assert(sizeof(void*)==8)` dans `src/app/main.cpp` ; toolchain `cmake/toolchains/aarch64-linux-gnu.cmake` ; profil `profiles/pi-zero2.env` (`set -a; . profiles/pi-zero2.env; set +a`).
- Non fait (à mesurer d'abord sur le vrai Pi) : cache d'assets/rendu, pack Python, gamepad SDL GameController, launcher, overlay tactile, LVGL.

## Launcher (`src/app/launcher.cpp`)

- `./j2me_emu` sans argument ouvre le launcher (480x320, font 5x7 x2) : liste des `games/*.jar` triée (nom/éditeur/version du manifeste + `[LxH tourne]` lu dans le `.conf` voisin). Haut/Bas, Gauche/Droite (page), molette, Entrée/F1 = jouer, double-clic sur une ligne = jouer, F12/Ctrl+Q = quitter. `JME_GAMES_DIR=dossier` (défaut `games`), `JME_LAUNCHER=0` = ancien défaut `games/assasin.jar`, `JME_LAUNCHER_AUTO=n` = lance le jeu n° n à la 3e trame (CI).
- Le jeu est lancé par **re-exec du même binaire** (`/proc/self/exe`, Linux) : chaque jeu démarre dans un processus propre (pas de reset de Runtime/heap/globales MIDP). `JME_FROM_LAUNCHER=1` + `JME_LAUNCHER_LAST` (présélection) sont posés dans l'environnement. En jeu : **F12 = retour au launcher** (`InputState.exitToMenu`), fin du MIDlet (`notifyDestroyed`) aussi ; **Ctrl+Q / fermer la fenêtre = quitter pour de bon**. Les variables posées par le `.conf` du jeu sont retirées de l'environnement avant de relancer le launcher (sinon elles fuiraient sur le jeu suivant). Pas de retour au menu si `JME_MAXFRAMES` est défini.
- `hal::display_draw_text_scaled/fill_rect/dump_ppm` ajoutés pour le launcher.

- **Invocation sur récepteur `null` = vraie `NullPointerException`** (`interpreter.cpp`, cas invoke 0xb6/0xb7/0xb9) : avant, l'appel échouait simplement et tuait toute la chaîne d'appels, sans passer par les `catch` de l'appelant. Ferrari World Championship (`ferrari_world_champi_137653_2195.jar`) fait `try { in.close(); } catch (Exception e) {}` sur un flux jamais ouvert : le chargement échouait en cascade (écran noir). Ce jeu est un build **Nokia 96x65** (`.conf` : `JME_WIDTH=96 JME_HEIGHT=65`) ; la fenêtre des petits écrans (< 200 px) est agrandie automatiquement (jusqu'à x8).

## Fenêtre PC et audio (session du 26/09)

- **Taille de fenêtre par défaut** : plus grand facteur entier tenant dans ~90 % de la zone utile du bureau (`SDL_GetDisplayUsableBounds`) — 240x320 → x3 (720x960) sur un écran 1080p, le launcher 480x320 → x3. `JME_WINDOW_WIDTH/HEIGHT` prime. Redimensionnable (image proportionnelle + bandes noires). **F11 / Alt+Entrée = plein écran** (`display_toggle_fullscreen`). Titre de fenêtre = nom du MIDlet. La fenêtre affiche `[display] fenetre WxH pour une image WxH` sur stderr.
- **Volume** : F9/F10 (ou PgBas/PgHaut) = -/+10 %, F8 = muet, avec un petit OSD ~1,5 s (`kernel::audio::setMasterVolume`, géré dans `main.cpp`).
- **Diagnostic audio** : `JME_AUDIO_TEST=1` joue un bip 440 Hz de 0,8 s au démarrage ; `JME_AUDIO_CAPTURE=fichier.raw` enregistre le PCM réellement mixé (s16 mono 22050 Hz) — permet de vérifier sans écouter qu'un jeu produit du son (`python3 -c "import array;a=array.array('h');a.frombytes(open('f.raw','rb').read());print(max(a))"`). Le log `[audio] device SDL (pipewire)` donne le backend SDL effectif. Vérifié : Prince of Persia 2 et Assassin's Creed 2 (après « Do you want sound? » → YES) produisent un signal non nul (pic ~12000-19000) ; Ferrari/Mission/Checkers/Maze/Nokia Bounce n'appellent jamais `Player.start` dans les 400 premières trames.

## Session « 10 jeux open source » (26/09) — bugs trouvés en testant des JAR libres

JAR de test (licences libres, téléchargés dans `games/os_*.jar`, dossier gitignoré) : mobapp-game, Super FX-BALL, nmania, Shooter, Sudoku, Tetris, jtReversi, Sperm Race, Warehouse, Pacman, RetroSnake, 2048, Minesweeper. (Comcraft HD : manifeste faux `ComcraftMIDle`≠`ComcraftMIDlet` ; Akatis : ressource `l0trl` absente du JAR — JAR cassés, écartés.)

- **`midp::tick()` sortait dès qu'il n'y avait pas de Displayable courant** → les threads/minuteries ne tournaient JAMAIS pour les MIDlets qui démarrent un thread dans `startApp()` et appellent `Display.setCurrent()` depuis ce thread (Super FX-BALL, Sperm Race) : écran noir éternel. Les fibres tournent maintenant toujours.
- **Résolution des champs d'instance depuis la classe du `Fieldref`, pas le type d'exécution** (`getfield/putfield`, `resolveInstField`) : deux champs privés `a:Z` (superclasse + sous-classe, obfuscation) tombaient sur le même slot (mobapp-game : « already started »).
- **`instanceof`/`checkcast` par NOM** (super-chaîne + interfaces déclarées, `classIsA`) : avant, cible non encore chargée ⇒ « vrai » (chargement paresseux), puis `getfield` sur une mauvaise classe. Strings/Object gérés.
- **Opcodes manquants** : `fadd/fsub/fmul/fdiv/frem/fneg`, versions `d*`, `dup2_x1/dup2_x2`, `jsr/jsr_w/ret` (finally des vieux javac) — tests dans `tests/test_interpreter.cpp`.
- **`InputStream.read(byte[],off,len)` renvoyait 0 en fin de flux au lieu de -1** → boucle infinie de lecture de ressource (Warehouse).
- **Invocation sur `null`** = vraie NPE (voir plus haut). **Garde-fou anti-boucle infinie** sur les appels synchrones (`startApp` 1,5 G d'instr., callbacks paint/touches 200 M par trame) : la fenêtre ne peut plus se figer. Attention : SDL intercepte SIGTERM (poste un `SDL_QUIT` que la boucle ne lit pas si elle est bloquée) → utiliser `timeout -s KILL` pour les runs de test.
- **`[stub]`** : une méthode absente d'une classe native (API MIDP/CLDC partielle) est journalisée une fois (`[stub] Classe.méthode absente…`) et renvoie 0/null au lieu d'avorter la chaîne d'appels. `JME_STRICT=1` = ancien comportement (échec dur) pour lister les trous de l'API. Ne s'applique pas aux classes du JAR dont la racine native est `Object`.
- **CLDC 1.1 complété** (`src/cldc/natives_extra.inc`, inclus par `natives.cpp`) : String (`hashCode`, `getBytes`, `lastIndexOf`, `replace`, `valueOf(*)`, `new String(String|char[])`…), StringBuffer (`insert`, `reverse`, `deleteCharAt`…), Math (trigo, `exp`, `log`, F/D, et **`pow` corrigé** : le 2e double est en `args[2]`, un long/double occupe 2 slots), Integer (radix, `toHexString`…), classes `Long/Short/Byte/Character/Float/Double/Boolean`, `Enumeration` (`Vector.elements()`, `Hashtable.keys()/elements()`), `Stack`, `Date/Calendar/TimeZone`, `Thread(Runnable,String)`, `RecordStore.listRecordStores`, `Font.getDefaultFont`, `Canvas.hasPointerEvents`…
- `.jad` voisin (`<jeu>.jad`) : complète le manifeste (MIDlet-1) et expose ses attributs via `getAppProperty`; sans MIDlet-1 nulle part, repli = première classe qui étend `MIDlet` (`[manifest] MIDlet-1 absent : classe MIDlet detectee`).

## lcdui haut niveau (`src/midp/midp_lcdui.cpp`) — Alert / List / Form / Items / TextBox / Commands

Avant : `Form`, `List`, `Alert`, `TextBox`, `ChoiceGroup`… n'étaient que des stubs (une `Alert` s'affichait en écran NOIR, un `Form` ne montrait que ses `String`). Maintenant, vrai modèle + rendu + navigation :
- **État côté hôte**, indexé par le pointeur de l'objet Java (`Cmd`, `It` = Item, `Scr` = Displayable, `Chc` = modèle de Choice partagé List/ChoiceGroup). Le tas est un bump allocator sans GC : les pointeurs sont stables jusqu'à `reset()`.
- **Rendu** direct dans le framebuffer HAL (police 5x7 agrandie x1..x3 selon la largeur), titre + corps défilant + barre de touches programmables. **Alert** : boîte centrée, ferme sur FIRE/softkey ou après `timeout` (3 s par défaut ; `Alert.FOREVER` = jamais), puis retourne au displayable « next » ; `Alert.DISMISS_COMMAND` est livrée si l'alerte a un listener. **Form** : StringItem (wrap, boutons/liens), ImageItem, TextField (saisie clavier SDL_TEXTINPUT, contraintes NUMERIC/PHONENUMBER/DECIMAL/PASSWORD), ChoiceGroup (EXCLUSIVE/MULTIPLE/POPUP), Gauge (interactive ou indéfinie), Spacer, DateField ; `ItemStateListener.itemStateChanged` et `ItemCommandListener` appelés. **List** IMPLICIT/EXCLUSIVE/MULTIPLE avec `SELECT_COMMAND`. **TextBox** : saisie texte.
- **Commands** : 2 touches programmables (SOFT1 = F1, SOFT2 = Échap/F2). ≤ 2 commandes : gauche/droite (les BACK/CANCEL/STOP/EXIT à droite) ; > 2 : « Menu » à gauche (liste FIRE/haut/bas) + la commande de sortie à droite. Souris : clic sur ligne/élément/barre.
- **Canvas avec Commands non plein écran** (`setFullScreenMode(false)`, défaut) : les touches programmables déclenchent `commandAction()` au lieu de `keyPressed(-6/-7)`, et une barre de commandes est dessinée par-dessus le canvas (`lcduiCanvasSoftKeys`/`lcduiCanvasOverlay`). En plein écran : comportement inchangé.
- Saisie texte : `hal::InputState.text/backspaces` (SDL_TEXTINPUT + Backspace) -> `midp::setTextInput()` avant `tick()`. Limité à l'ASCII.
- Non implémenté : `CustomItem` (paint), `Ticker` (ignoré), images de `List` réduites à un icône, `DateField` = libellé seul.
- Autres corrections du même lot : `AIOOBE extends IndexOutOfBoundsException`, `ClassNotFoundException`…, exceptions **levées depuis les natives** (`throwJava`, `NativeContext::exception`) : `NumberFormatException` (parseInt/Long/Float/Double strict), `StringIndexOutOfBoundsException` (charAt/substring), `ArrayIndexOutOfBounds`/`NoSuchElement` (Vector), `EmptyStackException`, `EOFException` (`DataInputStream.read*`), `ConnectionNotFoundException` (`Connector.open` : aucun réseau/fichier émulé), Sprite/TiledLayer MIDP 2.0 réels (tuiles 1-based, collisions pixel, rectangle de collision, transformations + pixel de référence), `Image.createImage(Image,x,y,w,h,transform)`, **arène de frames par fibre** (`Interpreter::swapArena`).

## Police et texte (`src/hal/font.cpp`)

- **Une seule police 5x7** pour tout (HAL, launcher, lcdui, `Graphics.drawString`) : ASCII + **Latin-1 composé** (À-ÿ : lettre de base + accent posé sur les 2 rangées du haut, cédille, ñ, ø, ß, ¡ ¿ ° ± × ÷ « » £ ¥ µ § ² ³ ¹ ...). Les `String` du projet restent des octets 8 bits (Latin-1) : cyrillique/thaï/CJK/polonais hors Latin-1 → `?`.
- `Font` : `getHeight()` = 8×échelle, `getBaselinePosition()` = 6×échelle, `stringWidth/charWidth` = 6×échelle par caractère, échelle = ×2 si l'écran fait ≥ 400 px de large, ×2 de plus pour `SIZE_LARGE` ; `STYLE_BOLD` (double frappe) et `STYLE_UNDERLINED` gérés. Ancres texte : `BASELINE`=0x40, `BOTTOM`=0x20 (avant : inversées).
- Encodages : `new String(byte[],"UTF-8")`, `getBytes("UTF-8")`, `DataInputStream.readUTF`/`DataOutputStream.writeUTF` transcodent UTF-8 ↔ Latin-1.

## Session « 20 jeux de plus » (26/09) — bugs trouvés

Jeux de test supplémentaires : `games/os2_*.jar` (19 JAR publiés par leurs auteurs sur GitHub ; licences variées ou non précisées — usage de test local uniquement, dossier `games/` non versionné).
- **Translation Graphics INVERSÉE** (`Pix::put`) : `g.translate(dx,dy)` dessinait à `(x-dx, y-dy)` au lieu de `(x+dx, y+dy)`, et le clip était testé dans le mauvais repère. Corrigé : le clip est en coordonnées utilisateur (translatées), la cible reçoit `x+tx`. En plus, `drawImage/drawRegion` repositionnaient une image ENTIÈREMENT hors écran pour en laisser une colonne visible (`x = 1 - w`) : supprimé (`put()` rogne déjà).
- **`flushGraphics()` cède la main à la trame suivante** (`jme_yieldNow()` depuis une fibre) : sur un vrai téléphone il est synchronisé sur l'affichage et beaucoup de boucles de jeu n'ont AUCUN `Thread.sleep` (Stalker : 300 images/s, une trame d'émulation prenait 0,5 s). `hal::display_present()` ne fait plus que marquer l'image ; `display_flip()` (une fois par trame, boucle principale + launcher) l'affiche (chaque `SDL_RenderPresent` attend le vsync).
- **Pas de GC** : une image recréée à chaque trame épuise le tas → `Image.createImage(String)` et `createImage(Image,x,y,w,h,transform)` sont mémorisés (images immuables, partage sans risque). Un vrai GC reste à faire (marquage conservateur : les `Value` n'ont pas de tag de type).
- **StackOverflowError** : `Interpreter::setStackLow` (limite basse de la pile C++ par exécution : principale −6 Mo, fibres +48 Ko ; piles de fibres 512 Ko) — une récursion trop profonde lève l'exception Java au lieu de segfaulter.
- **RMS complet** (`natives.cpp`) : `deleteRecord`, ids stables et jamais réutilisés, `getNextRecordID/getVersion/getSize/getSizeAvailable/getLastModified/getName`, `openRecordStore(name,create,auth,writable)`, `RecordStoreNotFoundException` (open sans create d'un magasin absent), `InvalidRecordIDException`, `RecordStoreNotOpenException`, `RecordStoreFullException`, comparateurs de `enumerateRecords` (tri stable via `compare([B[B)I`), `rebuild/keepUpdated`. Format disque v1 (dense, inchangé) ou v2 (`0xFFFFFFF2`, ids creux).
- **CustomItem** réel (`midp_lcdui.cpp`) : `paint(g,w,h)` appelé avec un Graphics dont l'origine est l'item et le clip = item ∩ corps visible ; `getPrefContentWidth/Height`, `traverse(dir,vw,vh,visRect)` (flèches), `keyPressed/keyReleased` (FIRE, 0-9, *, #), `pointerPressed/Released/Dragged` ; `getInteractionModes()` = 0xFF.
- Divers : `Graphics.drawChar(CIII)V` / `drawChars([CIIIII)V` (les descripteurs enregistrés étaient faux → aucun texte), `Class.forName/newInstance`, `java.io.InputStreamReader` (Latin-1/UTF-8), `Vector.trimToSize/ensureCapacity`, MIDI corrompu (octet de donnée ≥ 0x80 débordait un tableau de 128), `Image.createImage(...)` lève `IOException`/`IllegalArgumentException` au lieu de renvoyer une image blanche 1x1.
- **`Canvas.showNotify()` / `hideNotify()` jamais appelés** (`switchCurrent()` dans `midp_lcdui.cpp`, utilisé par tous les `setCurrent*`) : appelés quand un Canvas devient / cesse d'être l'écran courant, `showNotify()` AVANT le premier `paint()`. BluWar ne démarre son thread de rafraîchissement que dans `showNotify()` (écran figé sans cela) ; le Shooter passait de son splash à son menu.
- **Chaque `GameCanvas` a son PROPRE tampon hors écran** (`canvasGfx()`, `midp_graphics.cpp` : `uint16_t[w*h]` hors tas, blanc au départ comme dans la spec, pointeur brut rangé dans la cellule `G_BUF` de son `Graphics`). Avant : un seul tampon partagé, effacé à chaque `setCurrent` → le dessin fait dans le constructeur d'un canvas (écran d'accueil de SnakeWar) disparaissait. `GameCanvas.paint()` par défaut (pas de surcharge bytecode) **affiche** ce tampon (`presentGameCanvasBuffer`).
- **`pop2` ne retirait qu'un slot** quand le sommet n'était pas un long/double : `pop2` sur deux `int` (motif `dup2_x1; pop2; iastore` de Yet Another Snake) perdait le tableau visé → NPE. Test de régression `interpreter_pop2_pops_two_slots_for_ints_and_one_long`.
- **`Class.getResourceAsStream("images/x.png")` sans `/` initial est relatif au paquetage de la classe** (2048 : `game2048/scene/images/logo.png`). On essaie le chemin de paquetage puis la racine du JAR (indulgence).
- **`midp::tick()` itère sur une COPIE de la liste des threads** : un `Thread.start()` fait pendant `run()`/`showNotify()` invalidait les itérateurs (plantage dans `tick`).

## Cadence et horloge du jeu (session du 26/09, « saccades, tantôt rapide tantôt lent »)

Cause : `Thread.sleep(ms)` ignorait `ms` (une trame par itération) et l'horloge du jeu avançait de 16 ms fixes par trame de 33 ms réelles. Les jeux rythmés par `sleep` tournaient trop vite, ceux rythmés par `currentTimeMillis` deux fois trop lentement, et toute trame lente ralentissait tout.
- **Horloge du jeu = temps RÉEL** (`midp::tick`, µs, `virtualMicros()`), bornée à 100 ms par trame. `JME_FRAME_TIME=ms` garde un pas FIXE déterministe (CI, comparaisons pixel-à-pixel : à poser pour les tests).
- **`Thread.sleep(ms)` / `Object.wait(ms)` honorés** (`sleepFiber`, `SchedState` dans `natives.cpp`) : ordonnanceur par événements (`jme_schedNext`), un thread endormi est repris à son instant exact dans la trame (max 6 reprises/trame). `notify` réveille un `wait(ms)`. Plancher `JME_MIN_SLEEP` (défaut 33 ms) : les `sleep(5)` des jeux réglés pour des téléphones lents ne les font pas tourner 6x trop vite. `JME_SLEEP=frame` = ancien comportement (vérifié pixel-identique sur 50 jeux).
- **`flushGraphics()`** (`jme_flushYield`) : cède la main avec ≥ 33 ms entre deux flush, sauf si la boucle dort déjà entre deux flush.
- **Cadence de la boucle principale** : 16 ms (~60 trames/s) ; si le vsync est actif (`display_vsync_active()`), aucune attente ajoutée (elle donnait 33/50 ms en alternance). `JME_FRAME_BUDGET` force une période.
- Outils : `JME_RENDER_STATS=1` (durées de trame p50/p95/max, trames lentes), `JME_PROFILE=1` (profil d'échantillonnage des méthodes Java), `JME_SLEEPDBG=1` (sleeps/s en temps de jeu vs réel). Les mesures de temps sont très bruitées si le PC est chargé (VS Code, navigateur).

## Vitesse de l'interpréteur (session du 26/09, 2 à 4x plus rapide)

Mesuré sur `prince_of_persia_th` (100 trames, budget d'instructions fixe, temps CPU utilisateur) : 2,7 s → ~1,0 s. Sortie **pixel-identique sur les 50 jeux**. Les gains, par ordre d'importance :
- **Natives : clé string + hachage à chaque appel.** `dispatch()` construisait `"classe.méthode:desc"` (3 concaténations) et cherchait dans la table à CHAQUE appel natif (~25 M fois en 150 trames). La fonction résolue est maintenant mémorisée dans `MethodRecord::nativeFn` (pointeur stable vers le nœud de la table, `findNativePtr`). Vaut aussi pour les natives enregistrées après coup : toutes le sont avant la première exécution.
- **Caches par index de pool** (`ClassInfo::classRefCache`, `ldcCache`) : `new`, `checkcast`, `instanceof` (dernier type de receveur → résultat mémorisé) et `ldc` String ne refont plus de `std::string` par valeur + recherche par nom à chaque exécution.
- `execBytecode` ne remet plus à zéro la pile d'opérandes (seulement les locales) ; `ensureInit` n'est plus appelé quand `clinitDone` (invokestatic/invokevirtual/new).
- **Dispatch « threaded »** (`NEXT()` + table `jt`, GCC labels-as-values) : ~114 blocs d'opcodes fréquents se terminent par un saut indirect direct au lieu de repasser par le `switch` central. Gain modeste (~4 %) : le vrai coût est réparti (push/pop avec vérification de bornes, appels). Les opcodes non listés (retours, athrow, wide, switch…) reviennent au `switch` classique via `do_switch`. Ajouter un opcode : mettre `NEXT();` à la place du `break;` final de son `case` et l'ajouter à la table (`jt`).
- **Profilage** : `JME_PROFILE=1` (méthodes Java) ; `JME_PROFILE=rip` (adresses d'instruction C++, à traduire avec `addr2line -f -C -e binaire` sur un binaire `-g -no-pie`). Les mesures CPU sont bruitées quand le PC est chargé : comparer des binaires A/B **entrelacés**, minimum de plusieurs passes.
- Reste (si besoin de plus) : vérifications de bornes dans `push`/`pop`, budget d'instructions gardé en registre, superinstructions (iload+getfield…), tableaux d'octets stockés en `Value` de 8 octets.

## Jeux qui ne marchaient pas et marchent maintenant (27/09)

- **Gangstar Rio** (`gangstar_rio_city_o_260851.jar`) : « Do you want sound? » → menu → **Instant Play jouable** (HUD, minicarte, joystick tactile). La cause du menu jamais dessiné était `Canvas.showNotify()` jamais appelé (voir plus haut). Séquence de test (rotation 90° : coordonnées du framebuffer 480x800) : `JME_FRAME_TIME=33 JME_AUTOTOUCHES="99,320,1300;99,320,1500;99,320,1700;99,320,1900;99,320,2100;239,250,2700;239,250,2900" JME_MAXFRAMES=4500` (~5 min de CPU, la majorité pour décompresser les niveaux).
- **`GameCanvas(boolean suppressKeyEvents)`** rangeait son argument dans `GC_FULLSCREEN` : tout `GameCanvas(true)` perdait sa barre de commandes (SnakeWar : écran d'accueil sans moyen d'ouvrir le menu, touche programmable gauche = « Menu »). L'argument n'est plus utilisé pour le plein écran.
- Gangstar 2 (menu + « Do you want sound? »), SnakeWar (formulaire de configuration), BluWar, 2048, Yas : jouables. `checkers.jar` : JAR incomplet (`images/blackCrown.png` absent), l'`IOException` est légitime.
- **`nmania` n'était PAS bloqué par `FileConnection`** (correction de la note précédente) : il était juste
  impossible à faire avancer en headless au-delà de l'écran « What's your name? » (aucun outil pour taper
  du texte dans un `TextField`/`TextBox` scripté). `JME_AUTOTEXT="texte"` + `JME_AUTOTEXTFRAME=n`
  (`src/app/main.cpp`) injectent le texte comme si tapé au clavier (`midp::setTextInput`) à la trame `n` — le
  jeu atteint ensuite son menu principal (Play solo/Skinning/About). Au-delà, il essaie bien de scanner un
  dossier réel (`Connector.open("file://...")`, `javax.microedition.io.file.FileConnection`, non émulé) pour
  lister ses chansons/charts — mais le gère PROPREMENT : `ConnectionNotFoundException` est rattrapée et
  affiche un écran « Failed to load charts! ... Visit settings section... », pas un plantage. Émuler
  `FileConnection` (lister un vrai dossier de morceaux) resterait à faire si on veut du contenu jouable, mais
  ce n'est plus un blocage silencieux.
- Restent : AC III (texte thaï), texte hors Latin-1 (affiché « ? »), pas de GC.

## `newarray`/`anewarray`/`multianewarray` avec une taille NÉGATIVE (27/09)

`newarray`/`anewarray` passaient une taille d'array **négative** directement à `Heap::newArray` sans passer
par la spec JVM (`NegativeArraySizeException`) : le `int32_t` négatif, réinterprété comme taille, déclenchait
une tentative d'allocation énorme (gaspille/épuise le tas en essayant de grossir avant d'échouer), PUIS
échouait « en dur » (`okResult=false`) sans jamais passer par le `catch` de l'appelant — alors que du vrai
bytecode Java gère couramment ce cas (`try { ... new int[n] ...} catch (NegativeArraySizeException e) {}`)
pour une taille calculée à partir d'une ressource corrompue/tronquée. Trouvé sur
`games/ferrari_world_champi_137653_2195.jar` : `CAR.pak` (254 octets, visiblement un modèle de voiture
tronqué/factice) fait calculer une taille négative, et le jeu s'arrêtait au splash au lieu d'atteindre son
écran « Enable Sound? ». Corrigé dans `interpreter.cpp` (0xbc/0xbd/0xc5) : une taille négative (n'importe
laquelle des dimensions pour 0xc5) lève maintenant une vraie `NegativeArraySizeException` via `raiseJava`,
rattrapable. Test de régression :
`interpreter_newarray_negative_size_throws_catchable_exception`.

## `JME_AUTOTEXT`/`JME_AUTOTEXTFRAME` (27/09)

`JME_AUTOTEXT="texte"` + `JME_AUTOTEXTFRAME=n` (`src/app/main.cpp`) injectent du texte tapé (comme
`SDL_TEXTINPUT`, via `midp::setTextInput`) à la trame `n` : permet de traverser en headless les écrans de
saisie (nom de joueur...) qu'aucun softkey/clic scripté ne peut remplir. Sans effet quand la variable est
absente (comportement inchangé sur les 50 jeux de test).

## Ramasse-miettes (session du 27/09) — marquage-balayage conservateur, sans compaction

Le tas était un allocateur bump PUR : `reset()` (jamais appelé par l'app réelle, seulement par les tests)
était la SEULE récupération. Un GC réel est maintenant en place (`src/core/runtime.h`/`.cpp`, classe `Heap`).

- **Principe** : marquage-balayage NON DÉPLAÇANT (pas de compaction — un `Obj` embarque un `std::string` non
  relocalisable par `memcpy`, et de très nombreux `Obj*` bruts vivent côté C++ natif : caches lcdui, fibres,
  threads, sons... les déplacer casserait tout sans réécrire chaque référence). Les objets morts rejoignent
  une **liste de blocs libres** (premier ajustement, fusion des blocs adjacents à chaque cycle) réutilisée par
  `allocObj` avant de faire grossir le tas.
- **Marquage CONSERVATEUR** : les `Value` n'ont pas de tag de type (un `long`/`double` partage le même mot
  qu'une référence), donc impossible de savoir avec certitude, en regardant une pile Java ou une structure C++
  native, quels mots sont de VRAIES références. Chaque mot de 8 octets d'une région scannée est testé comme
  candidat contre `liveSet_` (l'ensemble des adresses d'objets ACTUELLEMENT alloués, reconstruit au début de
  chaque cycle en parcourant les segments par en-têtes) : un entier qui coïncide par hasard avec une adresse
  réelle ne fait que le garder vivant un cycle de trop (fuite bénigne) — **jamais** de corruption, puisqu'un
  mot n'est déréférencé QUE s'il correspond à une entrée de `liveSet_` (l'ordre de la vérification est
  crucial : membership AVANT tout déréférencement, pas l'inverse — un buggy `Value::fromInt(42)` scanné et
  interprété comme `Obj* 0x2a` déréférencé en premier plantait immédiatement).
- **Racines** : piles/locales Java de l'exécution active (`Interpreter::scanActiveFrames`, arène courante),
  celles de CHAQUE fibre suspendue (`jme_gcScanThreadingRoots`, natives.cpp — la fibre ACTIVE est couverte
  autrement, voir plus bas), `statics` de chaque classe chargée (`Runtime::forEachClass`), tout ce que retient
  nativement la couche MIDP (écrans lcdui, caches d'images, sons, minuteries — un `gcMark<Module>Roots` par
  fichier `midp_*.cpp`, orchestrés par `midp::gcMarkRoots`), et les caches internes du `Heap` lui-même
  (`internTable_`/`classCache_`, marqués directement dans `collectGarbage()`).
- **Piège trouvé et corrigé en cours de route (le plus important)** : un handler natif qui alloue un objet, le
  garde un instant dans une variable C++ **locale** (avant de l'écrire dans un champ/tableau Java), puis
  alloue ENCORE avant de s'en servir (ex. `img_createWH` : `Obj *buf = newArray(...)` puis d'autres allocations
  avant que `buf` ne soit rattaché à l'Image) n'exposait cette variable à AUCUNE racine — seul un balayage de
  la pile C++ elle-même la voit. Sans ça, un cycle GC déclenché par la deuxième allocation pouvait libérer PUIS
  RÉUTILISER la mémoire du premier objet pendant que le handler s'apprêtait encore à écrire dedans (repéré avec
  `JME_GC_STRESS=1` : corruption silencieuse du tas, `free(): invalid pointer` ou segfault bien plus tard,
  loin du vrai site fautif — la signature classique documentée plus haut dans ce fichier pour d'autres bugs).
  Corrigé en scannant conservativement la pile C++ RÉELLEMENT active (celle de la fibre en cours si une fibre
  tourne — son `ucontext` a littéralement échangé le registre SP dessus, voir `jme_currentFiberStackTop` — sinon
  celle du fil principal), mais **bornée à 256 Ko** (`kStackScanWindow`, `src/app/main.cpp`) plutôt que de
  remonter jusqu'au sommet réel (8 Mo) : une fenêtre trop large contient de vieilles cases de pile ABANDONNÉES
  par des appels déjà retournés (jamais réécrites depuis) qui gardent le bit à bit d'un ANCIEN `Obj*` — mesuré
  sur `games/os_pacman.jar`, ça faisait quasiment tout garder vivant pour toujours (0 octet récupéré par
  cycle) malgré un tas exigu qui finissait quand même par s'épuiser. 256 Ko (des centaines d'appels C++
  imbriqués, bien plus que la profondeur d'un handler natif) restitue une vraie collecte.
- **Second piège corrigé** : `allocFromFreeList` découpait un bloc libre trop généreusement — quand le
  reliquat après découpe était trop petit pour héberger lui-même un en-tête `Obj` valide, il était donné "en
  plus" à l'objet alloué SANS agrandir sa taille déclarée (`cellCount`) : l'objet occupait alors physiquement
  plus d'octets que ce que `sizeof(Obj) + cellCount*sizeof(Value)` rapportait, désynchronisant le parcours du
  tas par en-têtes (le cycle suivant retombait au milieu du reliquat non comptabilisé au lieu du prochain
  en-tête réel → lecture d'un `std::string` composé d'octets de bourrage → SIGSEGV dans le hachage de la table
  d'internement). Corrigé : un bloc dont le reliquat serait `> 0` et `< sizeof(Obj)` est simplement IGNORÉ
  (bloc gardé intact, recherche du suivant) plutôt que de casser l'invariant taille-déclarée == taille-occupée.
- **Validation** : `JME_GC_STRESS=1` force un cycle GC complet à CHAQUE allocation (le pire cas possible —
  fait apparaître en quelques trames un marquage incomplet qui, en fonctionnement normal, ne se manifesterait
  qu'après un long moment de jeu) ; `JME_GC_DEBUG=1` (avec `JME_GC_STRESS`) ajoute des vérifications de bornes
  (objet/bloc libre qui déborderait de son segment → `abort()` immédiat avec diagnostic, au lieu d'une
  corruption qui ne se manifeste que bien plus tard). Testé sous stress sur 9 jeux variés (dont Prince of
  Persia, Assassin's Creed 2, Gangstar 2, Đột Kích) : plus aucun crash après les deux corrections ci-dessus.
  Sortie **pixel-identique aux 50 jeux de test** en fonctionnement normal (défaut : le GC déclenche rarement
  sous un tas non plafonné). Efficacité démontrée sous tas plafonné (`JME_HEAP=512 JME_HEAP_MAX=768`, 8000
  trames) : `games/mission.jar` récupère 80-190 Ko de VRAIES ordures par cycle (StringBuffer/concaténations
  de score, petits tableaux temporaires...) et tient largement plus longtemps qu'avant. Un jeu dont le tas
  s'épuise malgré le GC (ex. `prince.jar`, quasi rien à récupérer) a un working-set réellement trop gros pour
  le plafond — aucun GC ne peut inventer de la mémoire pour de l'état légitimement vivant (images mises en
  cache, notamment).
- **`JME_GC=0`** désactive le GC entièrement (comportement d'avant son introduction : le tas ne fait plus
  jamais que grossir) — filet de sécurité si un jeu se comporte mal après coup.
- 4 nouveaux tests unitaires (`tests/test_runtime.cpp`) : GC sans racines enregistrées = no-op, récupération +
  réutilisation d'espace, marquage transitif à travers le graphe d'objets, marquage conservateur retrouvant un
  pointeur enfoui dans des entiers quelconques.

## Police 5x7 : audit Latin-1 (session du 27/09) — repli « ? » réellement cassé

En auditant la table de la police à la demande de l'utilisateur (rendu de tous les glyphes 0x00-0xFF dans une
image pour inspection visuelle), deux bugs réels trouvés dans `src/hal/font.cpp` :
- **Le repli par défaut vers « ? » ne marchait pas** : `init()` faisait `copyOf(c, '?')` pour les 256 octets
  AVANT de charger la vraie table ASCII (`kAscii`) dans `g_glyph[0x3F]` -- le « ? » copié était donc encore
  entièrement VIDE à ce moment-là. Tout caractère jamais explicitement composé (toute la plage C1 0x80-0x9F,
  et en Latin-1 : ¶ 0xB6, ¼ 0xBC, ½ 0xBD, ¾ 0xBE) restait **invisible** au lieu d'afficher « ? » comme
  documenté et voulu -- pire qu'un caractère inconnu visible : du texte qui semble amputé de certaines
  lettres. Confirmé concrètement sur `games/os_retrosnake.jar` (menu polonais, « Prędkość » etc. : les ą ę ć
  ś ż ź ó ł ń polonais sont hors Latin-1/Latin-2 vs la table ici) -- avant : mots à trous ("PrÄ dkoÅ Ä") ;
  après : "?" visible à la bonne place ("PrÄ?dkoÅ?Ä?"). Corrigé en chargeant `kAscii` AVANT la boucle de
  repli (qui saute maintenant volontairement 0x20-0x7E pour ne pas écraser l'ASCII qu'elle vient de charger).
- **¶ (pilcrow) et les fractions ¼ ½ ¾ n'avaient aucun glyphe dédié** (tombaient dans le même bug ci-dessus) :
  ajoutés (approximations dessinées à la main, dans le style du reste du fichier -- chiffre/barre/chiffre pour
  les fractions, trop serré à 5 colonnes pour une vraie diagonale). ¸ (cédille isolée, 0xB8) avait le même
  trou (jamais gérée) : ajoutée sur le même principe que ´/¨ isolés déjà présents (0xB4/0xA8).
- Le reste de la table (lettres accentuées composées, marques grave/aigu/circonflexe/tréma/tilde/rond/cédille,
  Ø/ø, Ð/ð, Þ/þ, ß) a été vérifié un par un contre la vraie table ISO-8859-1 : aucune autre erreur de mapping
  trouvée. Æ/æ (0xC6/0xE6) restent une simple copie de A/a (perte de la ligature, connu, accepté -- 5 colonnes
  ne permettent pas grand-chose de mieux).
- Outil de vérification (non versionné, scratchpad) : petit programme qui appelle `hal::font_glyph()` pour
  0x00-0xFF et dessine une planche PNG -- à refaire si la police est retouchée à nouveau.
- Sortie **pixel-identique aux 50 jeux de test**, sauf `os2_retrosnake` (l'amélioration ci-dessus, attendue).
