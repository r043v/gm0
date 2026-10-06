# Build navigateur uniquement : wasm/index.html + wasm/gm0.js + .wasm
# (nécessite emsdk : source ~/emsdk/emsdk_env.sh, ou emcc déjà dans le PATH).
# Le build natif (Linux/macOS/Windows) passe par CMake :
#   cmake -B build . && cmake --build build   # -> build/gm0
EMCC ?= emcc
EMFLAGS := -O3 -flto -sUSE_SDL=2 -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
     -sSTACK_SIZE=1048576 \
     -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToUTF8,UTF8ToString -sEXIT_RUNTIME=0 \
     -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_pause,_emu_pause_toggle,_emu_paused,_emu_restart,_emu_card_file,_emu_card_finish,_emu_card_image,_emu_firmware,_emu_games_count,_emu_game_name,_emu_game_icon,_emu_select_game,_emu_zip_load

wasm: gm0.c
	$(EMCC) $(EMFLAGS) -o wasm/gm0.js gm0.c -lm
	@echo "wasm : servir wasm/ en HTTP (python3 -m http.server) et ouvrir index.html"

# version mono fichier : le wasm est embarqué en base64 dans le HTML
single: gm0.c wasm/index.html
	$(EMCC) $(EMFLAGS) -sSINGLE_FILE=1 -o wasm/gm0_single.js gm0.c -lm
	python3 wasm/make_single.py

clean:
	rm -f wasm/gm0.js wasm/gm0.wasm wasm/gm0_single.js wasm/gm0-standalone.html
.PHONY: clean wasm single
