cmake_minimum_required(VERSION 3.16)
project(j2me_emu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(j2me_emu
    main.cpp
    hal/jar_reader.cpp
    hal/inflate.cpp
    hal/file.cpp
    hal/display.cpp
    hal/input.cpp
    hal/png.cpp
    vm/class_file.cpp
    vm/interpreter.cpp
    vm/runtime.cpp
    vm/natives.cpp
    vm/midp_natives.cpp
    vm/midp_core.cpp
    vm/midp_graphics.cpp
    vm/midp_game.cpp
    vm/midp_ui.cpp
    vm/midp_io.cpp
    vm/midp_media.cpp
    kernel/kernel.cpp
    kernel/drivers/audio/audio.cpp
    kernel/drivers/audio/stub_audio.cpp
    kernel/drivers/audio/sdl_audio.cpp
)

target_include_directories(j2me_emu PRIVATE
    .
    hal
    vm
    kernel
)

target_link_libraries(j2me_emu PRIVATE ${SDL2_LIBRARIES})
target_compile_options(j2me_emu PRIVATE ${SDL2_CFLAGS_OTHER})

# Sur PC : index en RAM pour debug rapide. Retirer pour RP2040.
target_compile_definitions(j2me_emu PRIVATE JAR_READER_INDEX_IN_RAM)

# NOTE : la commande de compilation équivalente est :
# g++ -std=c++17 -O2 -I. -Ihal -Ivm -Ikernel -DJAR_READER_INDEX_IN_RAM \
#     main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
#     hal/display.cpp hal/input.cpp hal/png.cpp \
#     vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
#     vm/natives.cpp vm/midp_*.cpp \
#     kernel/kernel.cpp kernel/drivers/audio/audio.cpp \
#     kernel/drivers/audio/sdl_audio.cpp kernel/drivers/audio/stub_audio.cpp \
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
    hal/inflate.cpp
    hal/jar_reader.cpp
    hal/file.cpp
    hal/png.cpp
    vm/class_file.cpp
    vm/runtime.cpp
    vm/interpreter.cpp
    vm/natives.cpp
)
target_include_directories(j2me_tests PRIVATE . hal vm)
add_test(NAME j2me_tests COMMAND j2me_tests)


