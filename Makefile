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

clean:
	rm -f wasm/gm0_single.js wasm/gm0-standalone.html
.PHONY: clean wasm
