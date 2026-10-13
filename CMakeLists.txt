# gm0 — émulateur Gamebuino META (SAMD21 / Cortex-M0+) et Pokitto
# (LPC11U6x / Cortex-M0+) en C/SDL2, avec build WebAssembly via le Makefile
# (make wasm, emsdk).
cmake_minimum_required(VERSION 3.20)
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

if(EMSCRIPTEN)
    add_library(wasm_sdl INTERFACE)
    target_compile_options(wasm_sdl INTERFACE -sUSE_SDL=2 -sUSE_ZLIB=1)
    target_link_options(wasm_sdl INTERFACE -sUSE_SDL=2 -sUSE_ZLIB=1)
    add_library(SDL2::SDL2 ALIAS wasm_sdl)
    set(SDL2_FOUND ON)
    add_library(wasm_zlib INTERFACE)
    add_library(ZLIB::ZLIB ALIAS wasm_zlib)
else()
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
endif()

add_executable(gm0 gm0.c)

# User-provided game, loaded automatically when no game/card is supplied.
set(GM0_BUNDLE "" CACHE FILEPATH "Game to embed: .bin, .pop or .zip")
if(GM0_BUNDLE)
    get_filename_component(GM0_BUNDLE_ABS "${GM0_BUNDLE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(GM0_BUNDLE_EXT "${GM0_BUNDLE_ABS}" LAST_EXT)
    string(TOLOWER "${GM0_BUNDLE_EXT}" GM0_BUNDLE_EXT)
    if(NOT GM0_BUNDLE_EXT MATCHES "^\\.(bin|pop|zip)$")
        message(FATAL_ERROR "GM0_BUNDLE expects a .bin, .pop or .zip game")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_game.py
        "${GM0_BUNDLE_ABS}" "${CMAKE_CURRENT_BINARY_DIR}/gm0_bundle.h"
        COMMAND_ERROR_IS_FATAL ANY)
    target_include_directories(gm0 PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(gm0 PRIVATE GM0_EMBEDDED_GAME)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GM0_BUNDLE_ABS}" tools/embed_game.py)
endif()

# Interface en français (par défaut : anglais)
option(GM0_FR "French interface (default: English)" OFF)
if(GM0_FR)
    target_compile_definitions(gm0 PRIVATE GM0_FR)
endif()

# Messages temporaires à l'écran (OSD) : activés par défaut ; -DGM0_OSD=OFF
# compile sans (aucun code ni donnée de l'OSD dans le binaire)
option(GM0_OSD "Messages temporaires à l'écran (OSD)" ON)
if(NOT GM0_OSD)
    target_compile_definitions(gm0 PRIVATE GM0_NO_OSD)
endif()

# Windows : un seul gm0.exe, SDL2 liée statiquement et exécutable en -static.
# -DGM0_WIN_STATIC=OFF garde SDL2.dll à côté de l'exécutable.
set(GM0_SDL_STATIC OFF)
if(WIN32)
    option(GM0_WIN_STATIC "Windows : SDL2 liée statiquement, un seul gm0.exe" ON)
    if(GM0_WIN_STATIC AND TARGET SDL2::SDL2-static)
        set(GM0_SDL_STATIC ON)
    endif()
endif()

if(GM0_SDL_STATIC)
    target_link_libraries(gm0 PRIVATE SDL2::SDL2-static)
    target_link_options(gm0 PRIVATE -static)
elseif(TARGET SDL2::SDL2)
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

if(EMSCRIPTEN)
    set_target_properties(gm0 PROPERTIES OUTPUT_NAME gm0_single SUFFIX ".js")
    target_link_options(gm0 PRIVATE -sSINGLE_FILE=1 -sALLOW_MEMORY_GROWTH=1
        -sENVIRONMENT=web -sSTACK_SIZE=1048576 -sEXIT_RUNTIME=0
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToUTF8,UTF8ToString"
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_pause,_emu_pause_toggle,_emu_paused,_emu_restart,_emu_card_file,_emu_card_finish,_emu_card_image,_emu_firmware,_emu_games_count,_emu_game_name,_emu_game_icon,_emu_select_game,_emu_zip_load")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GM0_WEB_GAME "" CACHE FILEPATH "User-provided game to embed in the HTML")
    set(WEB_GAME_ARGS)
    if(GM0_WEB_GAME)
        get_filename_component(GM0_WEB_GAME "${GM0_WEB_GAME}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND WEB_GAME_ARGS --game "${GM0_WEB_GAME}")
    endif()
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gm0.html
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/wasm/make_single.py
            --js $<TARGET_FILE:gm0> --output ${CMAKE_BINARY_DIR}/gm0.html ${WEB_GAME_ARGS}
        DEPENDS gm0 wasm/index.html wasm/games.js wasm/make_single.py ${GM0_WEB_GAME}
        VERBATIM)
    add_custom_target(wasm_html ALL DEPENDS ${CMAKE_BINARY_DIR}/gm0.html)
endif()

# Windows Release : symboles et débogage retirés (SDL2 statique en embarque ~5 Mo)
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(gm0 PRIVATE -s)
endif()

# Windows, build croisé (cmake/mingw-w64.cmake) : DLL de dépendance à côté de gm0.exe,
# seulement si SDL2 n'est pas liée statiquement
if(NOT GM0_SDL_STATIC)
    foreach(dll IN LISTS GM0_WIN_DLLS)
        add_custom_command(TARGET gm0 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:gm0>")
    endforeach()
endif()

install(TARGETS gm0
    RUNTIME DESTINATION bin
)

option(BUILD_TESTS "Build frontend and device tests" OFF)
if(BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME embedded_games COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bundle.py)
    foreach(test filter osd flash nvic)
        add_executable(test_${test} tests/unit/test_${test}.c)
        target_link_libraries(test_${test} PRIVATE SDL2::SDL2 ZLIB::ZLIB)
        if(NOT WIN32)
            target_link_libraries(test_${test} PRIVATE m)
        endif()
        add_test(NAME ${test} COMMAND test_${test})
        set_tests_properties(${test} PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endforeach()
endif()
