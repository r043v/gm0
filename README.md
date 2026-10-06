# gm0 — Gamebuino META (SAMD21) **and Pokitto** (LPC11U6x) emulator, Cortex-M0+/M0 (C/SDL2 + WebAssembly)

> **Version française** : [README.fr.md](README.fr.md)

One emulator for both consoles: a silicon-faithful **ARMv6-M Thumb
interpreter** (a **Cortex-M0+** on the META, a **Cortex-M0** on the
Pokitto — cycle-counted, with the real peripherals: ports, SERCOM4/5,
DMAC, SysTick, TC4/TC5+DAC, NVIC; LPC-side SYSCON/CT32B/SCT/SSP on the
Pokitto), an SPI SD card (raw image or FAT16 folder built on the fly),
an SDL2 frontend and a WebAssembly build (same C core).  The target is
auto-detected at load time (META SAMD21 / Pokitto LPC11U6x), or forced
with `--target`.

## Project status

- **META (SAMD21)**: playable — homebrew games without the standard
  library (lapinou: screen DMA clocked by the SPI baud rate, custom SD
  and audio drivers), official-library games (Celeste in game, Cats &
  Coins, Picomon, Yatzy, Reuben Quest; "SD INIT OK", saves written),
  site loaders;
- **Pokitto (LPC11U6x)**: full boot (bit-bang LCD, timers, IAP, ROM API,
  persistent 4 KB EEPROM), native `.pop` container support, Pandemic
  rendered, Galaxy Fighters (.pop + music streamed from the card);
- **converted firmwares** (the converter is a separate project): the
  0.4.0+ DMA/SPI protocols (TC4 DMA audio, SD streaming) are followed;
- **web**: the same core compiles to wasm — served page or single-file
  standalone (file://, no network).

## Platform support

| Feature | Native (C/SDL2) | WebAssembly |
|---|---|---|
| Screen | resizable window, integer/fitted/stretched scaling (**F10**), fullscreen (**F11**) | canvas, crisp pixels, browser scaling |
| Sound | SDL2 output locked to the game timer (±0.5 % regulation) | WebAudio, starts on first gesture |
| Gamepad | SDL_GameController, hot-plug, labelled mappings | Gamepad API (through Emscripten's SDL port), same mappings |
| META keys | arrows/ZQSD/WASD, **Enter**=MENU, **Space**=A, **Ctrl**=B, **\***=HOME (or J/K/U/I) | identical |
| Pokitto keys | IJKL/arrows, **A/S/B/D/F** — or Enter=C, Space=A, Ctrl=B | identical |
| Pause / reset | **F5** = reset (no keyboard pause) | dock ⏸ (pause) and ⏹ (reset) buttons |
| Loading a game | CLI argument or drag & drop: `.bin`, `.img`, `.zip`, folder | drag & drop or 📄/📁: `.bin`, `.pop`, `.zip`, folder |
| SD card | folder → FAT16 on the fly (dynamic size), raw `.img`, `.zip` = full card; `.GB` streaming | `.zip` = full card; `.GB` streaming (in-memory FAT16) |
| Saves | `.SAV`/`.STA` written back to files; Pokitto EEPROM `<game>.eeprom` persisted | in memory — lost when the tab closes |
| Flash writes (loader auto-patch) | modelled | ignored |
| Speed % | window title (± raw % without pacing) | tab title + page corner |
| One-click games | — | right dock (`wasm/games.js`); single-file standalone usable from `file://` |
| Headless / capture | `--frames`, `--shot`, `--wav`, end-of-run `[bench]` line | — |
| Debugging | environment variables `EMU_*`, `SD_DEBUG`… | browser console |

## Performance

Absolute numbers depend on the host (CPU, RAM, OS, load) — the durable
fact is the median: both consoles run at **≈ 4× (META) and ≈ 3×
(Pokitto) real time** natively on a mid-range laptop, and the wasm build
holds real time in the browser (≈ 55 % of native on META, ≈ 33 % on
Pokitto).

## Features

- **drop** a `.bin` (firmware), a raw `.img`, a `.zip` (= full SD card,
  `.pop` accepted as firmware on Pokitto) or a **folder** (card only)
  onto the window; dynamic-size FAT16 card, `.SAV` files written by the
  game are written back to disk;
- resizable window (**F10**: scaling modes, **F11**: fullscreen),
  **F8** = Game Boy filter (4-shade DMG palette + dot-matrix grid), speed
  **%** in the title (unbounded: >100 = running too fast), debt-free
  deadline pacing, **F5** = reset;
- SDL_GameController gamepad (hot-plug) + keyboard (META and Pokitto);
- modified card export (`--out-img`), ignoring faulty flash writes
  (`-w/-W`), session audio WAV (`--wav`), screenshots (`--shot`),
  bounded headless runs (`--frames`);
- wasm: pause/stop/loading dock, offline game list (`wasm/games.js`),
  single-file standalone with games embedded in base64;
- debugging: per-address cycle profile (`EMU_PROF`), DMA/SPI/LCD/SD
  traces, input scripting (`EMU_INPUT`), FAT/flash/framebuffer dumps.

## Building

Native, via CMake (SDL2 + zlib required; Linux, macOS, Windows MinGW):

    cmake -B build .
    cmake --build build      # -> build/gm0

French interface: `cmake -B build-fr -DGM0_FR=ON && cmake --build build-fr`
(the default interface is English).

Browser (emsdk required, `emcc` on PATH): `make wasm` →
`wasm/gm0-standalone.html`, a single self-contained file (wasm embedded
in base64, usable straight from file://).

## Usage (native)

    gm0 [firmware.bin] [card] [--frames N] [--shot out.ppm] [--wav out.wav]

- with no argument the window opens empty: **drop** a game onto it;
- `<card>` = a `.img` image OR a folder (FAT16 built on the fly); for
  the META the card defaults to the firmware's directory, for the
  Pokitto it is always an explicit argument;
- META keys: arrows/ZQSD/WASD, **Enter**=Start (MENU), **Space**=A,
  **Ctrl**=B, **\***=Select (HOME), or J=A, K=B, U=MENU, I=HOME; the
  shift-register order follows the game's SPI speed
  (`EMU_BTN_ORDER=lapinou` forces the homebrew order);
- Pokitto keys: I/K/J/L or arrows, **A**=A, **S/B**=B, **D/C**=C,
  **F**=backlight — or META-style (Enter=C, Space=A, Ctrl=B);
- options: `--target meta|pokitto`, `--out-img <file>`, `-w [n]`, `-W`.

Headless (tests, captures):

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy gm0 \
      game.bin card --frames 300 --shot /tmp/shot.ppm

## Browser usage

Open `wasm/gm0-standalone.html` (double-click — no server needed, it
works from file://) or play directly at
<https://r043v.github.io/gm0/> (GitHub Pages, emulator only — drag &
drop your own games).

## Known limitations

- **Windows**: MSYS2 MinGW-w64 required (`dirent.h`); MSVC untested;
- **wasm**: flash writes are ignored (site-loader auto-patches stay
  inert) and `.SAV` saves are not exported across sessions (memory
  buffers);
- cards are built as **FAT16** (superfloppy or MBR depending on target);
- the repository contains **no games and no firmwares** (user content);
- tick-for-tick parity with the original TypeScript emulator was
  **deliberately dropped** (2026-10-05) in favour of real hardware.

## Todo

- export saves/EEPROM across sessions in wasm;
- flash writes in wasm (parity with native for loaders);
- multi-platform CI (Linux/macOS/Windows) and binary releases;
- real screenshots in this README;
- FAT32 for larger cards.

## Genesis & credits

This project is **100 % vibe-coded**: not a single line of C typed by
hand.  The whole emulator (≈ 5,800 lines of C, 66 commits) was written
by **GLM-5.3-Flash**, the ZCode agent, in one week — September 30 to
October 6, 2026.  Every commit in this repository is theirs, except one
co-signed by **Claude Opus 5.5** (the TC4-triggered DMA audio channel,
built in Claude's own tooling).  The counters for the whole development
— the converter was only ever touched to debug the emulator, so usage
reflects the emulator alone — straight from the session database:

- 14 sessions, 122 prompt messages;
- 5,446 model requests, 5,488 tool calls;
- **1.84 billion tokens** processed (1.82 B read from cache, ~4.2 M
  generated), ~52 h of cumulative model time.

Two sources had their **logic extracted**:

- **Andy O'Neill's TypeScript emulator** (MIT) — the first C port
  reproduced it tick for tick:
  [aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator);
- **Felipe Manga's PokittoEmu** — the LPC11U6x/Cortex-M0 core is a
  faithful C port of it:
  [felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu).

**Official datasheets** for everything else (real ARMv6-M core,
DMAC/SERCOM/TC/NVIC, SPI SD card, panel):

- SAM D21/DA1 — Microchip, DS40001882:
  <https://www.microchip.com/en-us/product/ATSAMD21G18>;
- LPC11U6x — NXP (datasheet + user manual UM10732):
  <https://www.nxp.com/docs/en/data-sheet/LPC11U6X.pdf>;
- ST7735 — Sitronix (NDA'd PDF, public copies abound);
- ARMv6-M Architecture Reference Manual:
  <https://developer.arm.com/documentation/ddi0419/latest>.

Plus **reverse-engineering of real hardware** — deep analysis and
debugging, no code copied:

- the **lapinou source** on META (hand-written screen/SD/audio drivers,
  no standard library): it revealed the screen's real DMA/SPI protocol,
  the TC4 clocking and the CS/pull-up quirks;
- **META binaries** and the **official META library**
  ([Gamebuino/Gamebuino-META](https://github.com/Gamebuino/Gamebuino-META)):
  stepped through in the emulator for the SdFat stack, the library's
  DMAC descriptors and TC5 audio.

Ecosystem: [gamebuino.com](https://gamebuino.com) on the META side;
[PokittoLib](https://github.com/pokitto/PokittoLib) on the Pokitto side.

## License

gm0 is licensed under **[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.fr)**
(© 2026 r043v): attribution, non-commercial, share-alike.

The logic extracted from two **MIT** emulators stays under MIT — their
notices are preserved in [LICENSE](LICENSE):
[aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(Andy O'Neill, 2017) and
[felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu)
(Felipe Manga, 2017).  Everything else carries no obligation:
implementing behaviour documented in datasheets (Microchip, NXP,
Sitronix, ARM) creates no derivative of those documents, and neither
the lapinou source nor META binaries nor the official library
contributed a single line of code (analysis only).
