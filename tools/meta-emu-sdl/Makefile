CFLAGS ?= -O3 -flto
meta_emu: meta_emu.c
	gcc $(CFLAGS) $(shell pkg-config --cflags sdl2 zlib) -o $@ meta_emu.c -lm $(shell pkg-config --libs sdl2 zlib)
# build navigateur : wasm/index.html + wasm/meta_emu.js + .wasm
# (nécessite emsdk : source ~/emsdk/emsdk_env.sh)
wasm: meta_emu.c
	source /home/m/emsdk/emsdk_env.sh >/dev/null 2>&1 ; \
	emcc -O3 -flto -sUSE_SDL=2 -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
	     -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToUTF8,UTF8ToString -sEXIT_RUNTIME=0 \
	     -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_pause,_emu_pause_toggle,_emu_paused,_emu_restart,_emu_card_file,_emu_card_finish,_emu_card_image,_emu_firmware,_emu_games_count,_emu_game_name,_emu_game_icon,_emu_select_game,_emu_zip_load \
	     -o wasm/meta_emu.js meta_emu.c -lm
	@echo "wasm : servir wasm/ en HTTP (python3 -m http.server) et ouvrir index.html"

# version mono fichier : le wasm est embarqué en base64 dans le HTML
single: meta_emu.c wasm/index.html
	source /home/m/emsdk/emsdk_env.sh >/dev/null 2>&1 ; \
	emcc -O3 -flto -sUSE_SDL=2 -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
	     -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToUTF8,UTF8ToString -sEXIT_RUNTIME=0 \
	     -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_pause,_emu_pause_toggle,_emu_paused,_emu_restart,_emu_card_file,_emu_card_finish,_emu_card_image,_emu_firmware,_emu_games_count,_emu_game_name,_emu_game_icon,_emu_select_game,_emu_zip_load -sSINGLE_FILE=1 \
	     -o wasm/meta_emu_single.js meta_emu.c -lm ; \
	python3 wasm/make_single.py

clean:
	rm -f meta_emu wasm/meta_emu.js wasm/meta_emu.wasm wasm/meta_emu_single.js wasm/meta-emu-standalone.html
.PHONY: clean wasm single
