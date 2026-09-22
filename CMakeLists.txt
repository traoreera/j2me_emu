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
)

target_include_directories(j2me_emu PRIVATE
    .
    hal
    vm
)

target_link_libraries(j2me_emu PRIVATE ${SDL2_LIBRARIES})
target_compile_options(j2me_emu PRIVATE ${SDL2_CFLAGS_OTHER})

# Sur PC : index en RAM pour debug rapide. Retirer pour RP2040.
target_compile_definitions(j2me_emu PRIVATE JAR_READER_INDEX_IN_RAM)

# NOTE : la commande de compilation équivalente est :
# g++ -std=c++17 -O2 -I. -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
#     main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
#     hal/display.cpp hal/input.cpp hal/png.cpp \
#     vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
#     vm/natives.cpp vm/midp_natives.cpp \
#     -o j2me_emu $(pkg-config --cflags --libs sdl2)


