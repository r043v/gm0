# gm0 — émulateur Gamebuino META (SAMD21 / Cortex-M0+) et Pokitto
# (LPC11U6x / Cortex-M0+) en C/SDL2, avec build WebAssembly via le Makefile
# (make wasm, emsdk).
cmake_minimum_required(VERSION 3.16)
project(gm0 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# -O3 -flto par défaut en Release (parité avec l'ancien Makefile natif)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_C_FLAGS_RELEASE "-O3 -flto")
        # GCC : une seule partition LTO — découpé en plusieurs, le
        # programme pouvait séparer step de sa boucle step_batch et perdre
        # l'inlining de l'interpréteur (un appel par instruction, -15-20 %)
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto-partition=one")
        endif()
    endif()
    add_compile_options(-Wall -Wextra)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(FATAL_ERROR
        "SDL2 introuvable - installez-le d'abord :\n"
        "  Debian/Ubuntu : apt install libsdl2-dev zlib1g-dev\n"
        "  Arch          : pacman -S sdl2 zlib\n"
        "  macOS         : brew install sdl2 zlib\n"
        "  Windows       : MSYS2 pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib")
endif()
find_package(ZLIB REQUIRED)

add_executable(gm0 gm0.c)

# Interface en français (par défaut : anglais)
option(GM0_FR "French interface (default: English)" OFF)
if(GM0_FR)
    target_compile_definitions(gm0 PRIVATE GM0_FR)
endif()

if(TARGET SDL2::SDL2)
    target_link_libraries(gm0 PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(gm0 PRIVATE SDL2::SDL2-static)
else()
    target_include_directories(gm0 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gm0 PRIVATE ${SDL2_LIBRARIES})
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(gm0 PRIVATE SDL2::SDL2main)
endif()

if(TARGET ZLIB::ZLIB)
    target_link_libraries(gm0 PRIVATE ZLIB::ZLIB)
else()
    target_include_directories(gm0 PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(gm0 PRIVATE ${ZLIB_LIBRARIES})
endif()

if(NOT WIN32)
    target_link_libraries(gm0 PRIVATE m)
endif()

install(TARGETS gm0
    RUNTIME DESTINATION bin
)
