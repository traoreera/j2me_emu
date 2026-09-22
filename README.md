# J2ME MIDP Emulator

A lightweight J2ME/MIDP emulator written in C++17, designed for both desktop development and eventual porting to an RP2040 target.

The project loads standard `.jar` MIDlets, reads their manifest, parses `.class` files, executes a subset of the JVM bytecode, bridges to native MIDP/CLDC APIs, and renders to an RGB565 framebuffer through a hardware abstraction layer.

## Highlights

- Loads and executes real J2ME/MIDP MIDlets from `.jar` files
- Parses class files and resolves runtime metadata
- Emulates a subset of the JVM execution model
- Implements native MIDP/CLDC APIs such as `Display`, `Canvas`, `Graphics`, and basic lifecycle handling
- Uses SDL2 on PC for display and input
- Keeps the design compatible with an embedded RP2040 port
- Avoids full file buffering and general-purpose allocations in hot paths

## Repository layout

- `main.cpp` – application entry point, JAR loading, runtime setup, and the frame loop
- `hal/` – hardware abstraction layer: file I/O, ZIP/JAR handling, inflation, PNG, display, input
- `vm/` – class parser, bytecode interpreter, runtime, native method registry, MIDP natives
- `games/` – sample MIDlet JARs used for manual validation
- `AGENTS.md` – repo-specific operational notes and constraints
- `CLAUDE.md` – implementation guidance and architecture notes
- `INTEGRATION.md` – RP2040 integration notes and CMake guidance
- `CMakeLists.txt` – desktop build configuration

## Build

The canonical build command matches the project expectations for CI and local development:

```bash
g++ -std=c++17 -O2 -I. -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
    main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
    hal/display.cpp hal/input.cpp hal/png.cpp \
    vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
    vm/natives.cpp vm/midp_natives.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```

Important note: the three include roots are required together:

```bash
-I. -Ihal -Ivm
```

This is because some files include project-root-relative headers while others use relative includes.

### CMake

```bash
mkdir -p build && cd build && cmake .. && make
```

The CMake setup enables `JAR_READER_INDEX_IN_RAM` for PC development. That setting is intended for desktop debugging and should be removed for the RP2040 target.

## Run

By default, the emulator runs the first game in `games/` if no argument is provided:

```bash
./j2me_emu
```

You can also target a specific MIDlet JAR:

```bash
./j2me_emu games/assasin.jar
./j2me_emu games/mission.jar
```

## Environment variables

The main loop reads a few runtime configuration variables in `main.cpp`:

- `JME_MAXFRAMES=n` – auto-quit after `n` frames; useful for headless or CI smoke tests
- `JME_AUTOKEY=5|0|*|#|FIRE|SOFT1|SOFT2|LEFT|RIGHT|UP|DOWN` – hold a key from frame 0
- `JME_AUTOKEYFRAME=n` – tap a key for a single frame at frame `n`
- `JME_DUMP=path.ppm` – dump the final framebuffer as a PPM image on exit
- `JME_WIDTH=n` / `JME_HEIGHT=n` – override the emulated screen resolution
- `JME_HEAP=n` – override the JVM heap size in KB (default is 512 KB)
- `SDL_VIDEODRIVER=dummy` – switch to headless mode

Example:

```bash
SDL_VIDEODRIVER=dummy JME_MAXFRAMES=20 ./j2me_emu games/assasin.jar
```

## Architecture overview

The project pipeline is:

```text
JAR / ZIP
  -> class file parser
  -> runtime / class loader
  -> bytecode interpreter
  -> native bridge
  -> HAL (display / input)
```

### `hal/`

This layer isolates the emulator from hardware details:

- `file.cpp` – low-level file access; currently uses `FILE*`/libc on PC and is the main porting target for RP2040
- `jar_reader.cpp` – reads ZIP/JAR entries and manifests; supports sequential central-directory scanning
- `inflate.cpp` – minimal DEFLATE decoder without an external zlib dependency
- `png.cpp` – PNG decoder for MIDlet resource images
- `display.cpp` – SDL2-backed framebuffer and text rendering
- `input.cpp` – keyboard mapping to J2ME key states

### `vm/`

This is the core JVM-like environment:

- `class_file.cpp` – parses `.class` files, constant pools, methods, fields, and bytecode attributes
- `runtime.cpp` – runtime objects, class metadata, heap, class loading, and native class registration
- `interpreter.cpp` – bytecode execution engine, method dispatch, and cooperative instruction budgeting
- `natives.cpp` / `midp_natives.cpp` – native methods and MIDP API glue

## Key design constraints

The project is intentionally written with embedded constraints in mind:

- No full-file buffering in hot paths; JAR and class I/O are stream-oriented
- No general-purpose JVM heap allocation in the interpreter hot path; heap is a bump allocator
- Output buffers for extraction and decompression are caller-provided rather than allocated internally
- Manifest parsing does not implement full RFC 822 folding support; this is acceptable for normal MIDlet metadata
- ZIP64 is intentionally unsupported; J2ME JARs are typically far below 4 GB
- Huffman decode remains bit-at-a-time to minimize RAM usage and complexity

These constraints are part of the architecture and should be preserved when making changes.

## RP2040 porting notes

The project is being designed to be portable to RP2040. The main hardware-specific piece today is the file layer:

- Replace `hal/file.cpp` with flash/SD-backed primitives
- Remove `JAR_READER_INDEX_IN_RAM`
- Add `JAR_READER_NO_COMMENT_SCAN` in embedded builds if needed to avoid a large stack reserve during ZIP comment scanning

See `INTEGRATION.md` for more details on the intended embedded build setup.

## Testing and validation

There is no automated test suite in this repository. Manual verification is the current method:

1. Build the emulator.
2. Run against a sample MIDlet, typically `games/assasin.jar` or `games/mission.jar`.
3. Confirm the output includes the expected MIDlet metadata and a clean shutdown message.
4. Use `JME_DUMP` for framebuffer inspection when needed.

Example headless validation command:

```bash
SDL_VIDEODRIVER=dummy JME_MAXFRAMES=20 ./j2me_emu games/assasin.jar
```

## Known caveats

- Some MIDP 2.0 classes or features are intentionally limited or not yet implemented
- The emulator is designed as a practical subset rather than a complete Java VM
- Certain MIDlets may depend on behavior that requires additional native API coverage
- Some games are sensitive to display size or timing and may require environment overrides such as `JME_WIDTH`, `JME_HEIGHT`, or custom key simulation

## License and status

This repository is a research and development emulator project focused on J2ME/MIDP compatibility and embedded-portability constraints. It is not a general-purpose Java runtime and is intended for targeted MIDlet testing and experimentation.

## Useful follow-up docs

- `AGENTS.md` – build and runtime notes
- `CLAUDE.md` – architecture, pitfalls, and developer guidance
- `INTEGRATION.md` – embedded porting details
