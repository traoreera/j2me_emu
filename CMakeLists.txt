cmake_minimum_required(VERSION 3.16)
project(j2me_emu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(j2me_emu
    src/app/main.cpp
    src/app/launcher.cpp
    src/hal/jar_reader.cpp
    src/hal/inflate.cpp
    src/hal/file.cpp
    src/hal/display.cpp
    src/hal/input.cpp
    src/hal/png.cpp
    src/core/class_file.cpp
    src/core/interpreter.cpp
    src/core/runtime.cpp
    src/cldc/natives.cpp
    src/midp/midp_natives.cpp
    src/midp/midp_core.cpp
    src/midp/midp_graphics.cpp
    src/midp/midp_game.cpp
    src/midp/midp_ui.cpp
    src/midp/midp_io.cpp
    src/midp/midp_media.cpp
    src/kernel/kernel.cpp
    src/kernel/audio/audio.cpp
    src/kernel/audio/stub_audio.cpp
    src/kernel/audio/sdl_audio.cpp
)

target_include_directories(j2me_emu PRIVATE src)

target_link_libraries(j2me_emu PRIVATE ${SDL2_LIBRARIES})
target_compile_options(j2me_emu PRIVATE ${SDL2_CFLAGS_OTHER})

# Sur PC : index en RAM pour debug rapide. Retirer pour RP2040.
target_compile_definitions(j2me_emu PRIVATE JAR_READER_INDEX_IN_RAM)

# NOTE : la commande de compilation équivalente est :
# g++ -std=c++17 -O2 -Isrc -DJAR_READER_INDEX_IN_RAM \
#     src/app/main.cpp
    src/app/launcher.cpp src/hal/*.cpp src/core/*.cpp src/cldc/*.cpp \
#     src/midp/*.cpp src/kernel/kernel.cpp src/kernel/audio/*.cpp \
#     -o j2me_emu $(pkg-config --cflags --libs sdl2)

# ---------------------------------------------------------------------
# Tests unitaires (tests/*.cpp, micro-framework maison, pas de gtest/catch2
# pour rester dans l'esprit "sans dependance lourde" du reste du projet).
# Volontairement PAS lie a SDL2/kernel : n'exerce que hal/vm cote logique
# pure (inflate, jar_reader, png, class_file, runtime, interpreter), donc
# buildable meme sans le paquet dev sdl2 installe.
# ---------------------------------------------------------------------
enable_testing()
add_executable(j2me_tests
    tests/test_main.cpp
    tests/test_inflate.cpp
    tests/test_png.cpp
    tests/test_class_file.cpp
    tests/test_runtime.cpp
    tests/test_interpreter.cpp
    tests/test_jar_reader.cpp
    tests/test_recordstore.cpp
    src/hal/inflate.cpp
    src/hal/jar_reader.cpp
    src/hal/file.cpp
    src/hal/png.cpp
    src/core/class_file.cpp
    src/core/runtime.cpp
    src/core/interpreter.cpp
    src/cldc/natives.cpp
)
target_include_directories(j2me_tests PRIVATE src)
add_test(NAME j2me_tests COMMAND j2me_tests)


