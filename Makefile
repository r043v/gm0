# Build navigateur : wasm/gm0-standalone.html — un seul fichier autonome
# (wasm embarqué en base64, utilisable en file:// ; les jeux listés dans
# wasm/games.js sont embarqués s'ils sont posés à côté).
# (nécessite emsdk : source ~/emsdk/emsdk_env.sh, ou emcc déjà dans le PATH).
# Le build natif (Linux/macOS/Windows) passe par CMake :
#   cmake -B build . && cmake --build build   # -> build/bin/gm0
EMCC ?= emcc
EMFLAGS := -O3 -flto -sUSE_SDL=2 -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
     -sSTACK_SIZE=1048576 \
     -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToUTF8,UTF8ToString -sEXIT_RUNTIME=0 \
     -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_pause,_emu_pause_toggle,_emu_paused,_emu_restart,_emu_card_file,_emu_card_finish,_emu_card_image,_emu_firmware,_emu_games_count,_emu_game_name,_emu_game_icon,_emu_select_game,_emu_zip_load

# options supplémentaires du build wasm, ex. sans OSD :
#   make wasm EXTRA_EMFLAGS=-DGM0_NO_OSD
EMFLAGS += $(EXTRA_EMFLAGS)

wasm: gm0.c wasm/index.html wasm/games.js wasm/make_single.py
	$(EMCC) $(EMFLAGS) -sSINGLE_FILE=1 -o wasm/gm0_single.js gm0.c -lm
	python3 wasm/make_single.py
	@echo "wasm : ouvrir wasm/gm0-standalone.html (file:// ou http)"

# Windows, build croisé depuis Linux (MinGW-w64), un seul gm0.exe (SDL2 statique).
#   paquets natifs (pacman, mingw-w64-gcc + mingw-w64-sdl2 + mingw-w64-zlib) : `make win`
#   sinon, sans root : `make win-setup` installe la chaîne dans $(MINGW_DIR)
# WIN_STATIC=OFF : SDL2.dll séparée à côté de gm0.exe.
MINGW_DIR ?= $(HOME)/.local/mingw-w64
WIN_BUILD ?= build-win
WIN_STATIC ?= ON
MINGW_SYSTEM ?= $(shell test -x /usr/bin/x86_64-w64-mingw32-gcc && echo ON || echo OFF)
ifeq ($(MINGW_SYSTEM),ON)
MINGW_ROOT ?= /
MINGW_GCC := $(MINGW_ROOT)/usr/bin/x86_64-w64-mingw32-gcc
else
MINGW_ROOT ?= $(MINGW_DIR)
MINGW_GCC := $(MINGW_DIR)/usr/bin/x86_64-w64-mingw32-gcc
endif

win-setup:
	GM0_MINGW_DIR=$(MINGW_DIR) sh tools/mingw-w64-setup.sh

win:
	@test -x "$(MINGW_GCC)" || { echo "chaîne MinGW introuvable ($(MINGW_GCC)) : installer les paquets natifs ou lancer make win-setup" >&2; exit 1; }
	cmake -S . -B $(WIN_BUILD) -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/mingw-w64.cmake -DGM0_MINGW_SYSTEM=$(MINGW_SYSTEM) -DGM0_MINGW_ROOT=$(MINGW_ROOT) -DGM0_WIN_STATIC=$(WIN_STATIC) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(WIN_BUILD) -j
	@echo "gm0.exe : $(WIN_BUILD)/bin/gm0.exe$(if $(filter OFF,$(WIN_STATIC)), (SDL2.dll copiée à côté),)"

clean:
	rm -f wasm/gm0_single.js wasm/gm0-standalone.html
.PHONY: clean wasm win win-setup
