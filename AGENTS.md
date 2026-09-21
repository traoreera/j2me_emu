# AGENTS.md — games (J2ME JAR reader + DEFLATE)

## Project overview
Minimal C++ library for reading JAR/ZIP files and decompressing DEFLATE streams.
Designed for **RP2040 (264 KB RAM)** — no dynamic allocation, streaming I/O via callbacks.

## Files
| File | Purpose |
|------|---------|
| `hal/jar_reader.h/cpp` | ZIP/JAR reader: locates EOCD, scans central directory, extracts entries |
| `hal/inflate.h/cpp` | DEFLATE (RFC 1951) decompressor: bit reader, Huffman decode, inflateStream |
| `hal/file.*` | File I/O abstraction (FILE* on PC, `hal_file_*` on RP2040) |
| `hal/display.*` | SDL2 RGB565 framebuffer 240x320 + 5x7 bitmap text (`display_draw_text`) |
| `hal/input.*` | SDL2 key mapping to J2ME keys (D-pad, softkeys, 0-9, # *) |
| `vm/class_file.*` | Class file parser: constant pool, fields, methods, Code attribute |

## Key design constraints (from headers)
- **No full file in RAM** — all I/O via `fseek`/`fread` through the HAL file layer (port to RP2040 by replacing `hal/file.cpp`)
- **No entry index by default** — sequential scan of central directory; compile with `-DJAR_READER_INDEX_IN_RAM` to enable RAM cache
- **Caller provides output buffers** — no internal allocation
- **No line-folding in manifest parser** — sufficient for MIDlet-* fields

## Build
```bash
g++ -std=c++17 -O2 -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
    main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
    hal/display.cpp hal/input.cpp vm/class_file.cpp \
    -o j2me_emu `pkg-config --cflags --libs sdl2`
```
Or via CMake (`CMakeLists.txt`, needs `sdl2` dev package).

## Usage example
```cpp
jme::JarReader jar;
if (!jar.open("game.jar")) return;

jme::ManifestInfo mi;
if (jar.readManifest(mi)) {
    // mi.mainClass, mi.midletName, etc.
}

uint8_t buf[16384];
size_t n = jar.extractClass(mi.mainClass, buf, sizeof(buf));
```

## Compile-time flags
| Flag | Effect |
|------|--------|
| `JAR_READER_INDEX_IN_RAM` | Build entry index in RAM (faster repeated lookups, more RAM) |
| `JAR_READER_NO_COMMENT_SCAN` | Disable ZIP comment scan (saves ~64 KB stack on RP2040) |

## Testing
No test suite exists. Verify manually with `games/assasin.jar` (`j2me_emu games/assasin.jar`).

## Porting to RP2040
Only `hal/file.cpp` uses `FILE*`. Replace it with your HAL (`hal_file_*` flash/SD primitives); everything else is HAL-agnostic.