# Toolchain croisé Linux -> Windows x86_64 (MinGW-w64) pour gm0, utilisé par
# `make win`.  Une seule racine GM0_MINGW_ROOT, deux dispositions :
#   - paquets natifs (GM0_MINGW_SYSTEM=ON, défaut /) : SDL2 et zlib dans
#     <racine>/usr/x86_64-w64-mingw32 (emplacement des paquets AUR) ;
#   - préfixe local (GM0_MINGW_SYSTEM=OFF, défaut ~/.local/mingw-w64, voir
#     tools/mingw-w64-setup.sh) : SDL2 et zlib dans deps/.
if(NOT DEFINED GM0_MINGW_SYSTEM)
    set(GM0_MINGW_SYSTEM OFF)
endif()
if(NOT GM0_MINGW_ROOT)
    if(GM0_MINGW_SYSTEM)
        set(GM0_MINGW_ROOT "/")
    else()
        set(GM0_MINGW_ROOT "$ENV{HOME}/.local/mingw-w64")
    endif()
endif()
set(GM0_MINGW_BIN "${GM0_MINGW_ROOT}/usr/bin")
set(GM0_MINGW_SYSROOT "${GM0_MINGW_ROOT}/usr/x86_64-w64-mingw32")
if(GM0_MINGW_SYSTEM)
    set(GM0_MINGW_SDL2 "${GM0_MINGW_SYSROOT}")
    set(GM0_MINGW_ZLIB "${GM0_MINGW_SYSROOT}")
else()
    set(GM0_MINGW_SDL2 "${GM0_MINGW_ROOT}/deps/sdl2/x86_64-w64-mingw32")
    set(GM0_MINGW_ZLIB "${GM0_MINGW_ROOT}/deps/zlib")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "${GM0_MINGW_BIN}/x86_64-w64-mingw32-gcc")
set(CMAKE_RC_COMPILER "${GM0_MINGW_BIN}/x86_64-w64-mingw32-windres")
# gcc-ar / gcc-ranlib chargent le plugin LTO (build -flto : archives d'objets LTO)
set(CMAKE_AR "${GM0_MINGW_BIN}/x86_64-w64-mingw32-gcc-ar")
set(CMAKE_RANLIB "${GM0_MINGW_BIN}/x86_64-w64-mingw32-gcc-ranlib")

# bibliothèques et en-têtes cherchés seulement dans la chaîne MinGW, jamais chez l'hôte
set(CMAKE_FIND_ROOT_PATH "${GM0_MINGW_SYSROOT}" "${GM0_MINGW_SDL2}" "${GM0_MINGW_ZLIB}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(SDL2_DIR "${GM0_MINGW_SDL2}/lib/cmake/SDL2")
set(ZLIB_ROOT "${GM0_MINGW_ZLIB}")
# zlib statique (libz.a) : le paquet natif fournit aussi libz.dll.a, que CMake
# choisirait sinon, et l'exécutable dépendrait de zlib1.dll
set(ZLIB_USE_STATIC_LIBS ON)

# DLL de dépendance, copiées à côté de gm0.exe seulement si WIN_STATIC=OFF (CMakeLists.txt)
set(GM0_WIN_DLLS "${GM0_MINGW_SDL2}/bin/SDL2.dll")
