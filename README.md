# gm0

**A cycle-counted emulator for the Gamebuino META (SAMD21, Cortex-M0+), the
Pokitto (LPC11U6x, Cortex-M0+) and the ESPboy (ESP8266, Xtensa LX106),
written in C with an SDL2 frontend and a WebAssembly build.**

> Version française : [README.fr.md](README.fr.md)

gm0 runs unmodified firmware for the three consoles from a single binary.  The
target is detected from the firmware image (initial stack pointer in SAMD21
or LPC SRAM; `0xE9` image header or ELF for the ESPboy) and can be forced with
`--target` (META and Pokitto).  The same C core is compiled
natively and to WebAssembly.

- **CPU** — ARMv6-M Thumb interpreter, cycle-counted per the Cortex-M0+
  timing tables, including NVM cache misses on the META.
- **META peripherals** — PORT, SERCOM4/5 (ST7735 display, SPI SD card,
  button shift register), DMAC (triggered channels, chained descriptors,
  suspend/resume), TC4/TC5, DAC, SysTick, NVIC with real priorities and
  exception stacking, minimal NVMCTRL.
- **Pokitto peripherals** — SYSCON/PLL, CT32B, SCT, SSP, GPIO, IAP and ROM
  API, persistent 4 KB EEPROM, native `.pop` container support.
- **ESPboy** — Xtensa LX106 interpreter running the firmware's own code (Arduino
  core, runtime, generated code); the chip's boot ROM and the closed Espressif
  SDK are *emulated by function* (see [ESPboy](#espboy)): ROM libc, soft
  floating point, `ets_*`, flash functions, timer1, GPIO and the sigma-delta
  audio output, SPI1 to the ST7735 (128×128), the MCP23017 buttons.
- **Storage** — SPI SD card backed by a raw image, a `.zip`, or a host folder
  converted to FAT16 on the fly; saves written by games are written back.

## Status

| Target | State | Tested titles |
|---|---|---|
| Gamebuino META | Playable | lapinou (bare-metal drivers), Celeste, Cats & Coins, Picomon, Yatzy, Reuben Quest, site loaders, converted Game Boy firmwares (DMA/SPI protocols 0.4.0+) |
| Pokitto | Playable | Pandemic, Galaxy Fighters (`.pop` with music streamed from the card) |
| ESPboy | Boots and plays m0boy firmwares (Game Boy, SG-1000, Game Gear, Master System) | firmwares converted by m0boy; not checked against a console |
| WebAssembly | Playable | same core; served page or single self-contained HTML file |

The repository ships no games or firmware.

## Building

Native build with CMake (requires SDL2 and zlib; Linux, macOS, Windows via
MSYS2 MinGW-w64):

```sh
cmake -B build .
cmake --build build          # produces build/bin/gm0
```

The interface is in English by default; configure with `-DGM0_FR=ON` for
French.  Short on-screen messages (reset, filter, scale, load) are on by
default; `-DGM0_OSD=OFF` removes them.

WebAssembly build (requires emsdk with `emcc` on `PATH`):

```sh
make wasm                    # produces wasm/gm0.html
```

The output is a single file with the wasm module embedded; it can be opened
directly from `file://`.  A hosted build is available at
<https://r043v.github.io/gm0/> (emulator only — load your own games).

Windows, cross-compiled from Linux with MinGW-w64, as a single `gm0.exe` (SDL2
linked in).  With the native packages installed (`mingw-w64-gcc`,
`mingw-w64-binutils`, `mingw-w64-crt`, `mingw-w64-headers`,
`mingw-w64-winpthreads`, `mingw-w64-sdl2` and `mingw-w64-zlib` from the AUR):

```sh
make win                     # build-win/bin/gm0.exe
```

Without them, `make win-setup` installs the same toolchain under
`~/.local/mingw-w64` (about 440 MB, no root), then `make win` works the same
way.  `make win WIN_STATIC=OFF` keeps SDL2 in a separate `SDL2.dll`.

### Embedded games

`main` runs META/Pokitto firmwares. Direct SG-1000 cartridge support lives on
[`z80`](https://github.com/r043v/gm0/tree/z80).

Build a native executable that starts your game without arguments:

```sh
cmake -G Ninja -S . -B build-bundle -DGM0_BUNDLE=/path/game.zip
cmake --build build-bundle
```

`GM0_BUNDLE` accepts a `.bin` or `.pop` firmware, or a ZIP containing the
firmware and its SD assets (including any ROM needed by the firmware).
Use a ZIP for games that read files from the card. A raw Game Boy ROM must
first be converted to a META/Pokitto firmware with m0boy. An explicit game
or card argument overrides the embedded game. Windows builds use static
SDL2 by default when available.

For a single HTML that starts the embedded game (emsdk required):

```sh
emcmake cmake -G Ninja -S . -B build-wasm -DGM0_WEB_GAME=/path/game.zip
cmake --build build-wasm     # build-wasm/gm0.html
```

`GM0_WEB_GAME` accepts the same game formats. The module and game are embedded
in the HTML for offline use; browser audio still requires a user gesture.
Keep user-provided games and generated builds outside tracked sources.

## Usage

```sh
gm0 [firmware] [card] [options]
```

| Argument | Meaning |
|---|---|
| `firmware` | `.bin`, or `.pop` (Pokitto) |
| `card` | `.img` raw image, `.zip` (full card), or a folder (FAT16 built on the fly). On the META the card defaults to the firmware's folder. |
| `--target meta\|pokitto` | force the target |
| `--rom file`, `--sym file` | ESPboy: ROM image (`rom.bin`, flash `0x100000`) and symbol file of a `.bin` (default: `rom.bin` and `<name>.sym` / `firmware.sym` next to it) |
| `--frames N` | run N frames, then exit |
| `--shot file.ppm` | write a screenshot at exit |
| `--wav file.wav` | record the session audio (48 kHz, rendered from emulated time) |
| `--out-img file.img` | export the card on exit if it was modified |
| `--eeprom-ro` | Pokitto: load the `.eeprom` next to the firmware but never write it back (reproducible runs) |
| `-w [n]`, `-W` | Pokitto: ignore the next *n* (or all) faulting flash writes instead of raising a HardFault |

Started without arguments, the window opens empty; games can be dropped onto
it (`.bin`, `.img`, `.zip`, folder).  The run ends with a `[bench]` line
reporting raw emulation speed.

Headless runs (tests, captures):

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  gm0 game.bin card/ --frames 300 --shot shot.ppm
```

### ESPboy

```sh
gm0 game.zip                         # firmware.bin + firmware.sym [+ rom.bin], as m0boy's compile.sh packs it
gm0 firmware.bin --rom rom.bin       # firmware.sym is looked up next to it
gm0 firmware.elf [--rom rom.bin]     # the ELF carries the symbols
```

The ESPboy build keeps the machine honest where the code *is* the firmware's
(Xtensa core, memory map with the chip's access rules: byte and halfword loads
from IRAM or the mapped flash stop the run, as `LoadStoreError` does on the
chip) and *approximate* where the code is Espressif's: the boot ROM and the SDK
are not shipped here, so what the firmware calls of them is emulated by
function.  Start-up (`eboot`, `app_entry`, `user_init`) is **not** run: the
machine does what it leaves behind (cont stack, timer1 ISR, C++ constructors)
and calls `setup()` then `loop()`.  Hooked by symbol name (`firmware.sym`, or the
ELF): `micros`, `delay`, `system_update_cpu_freq`, `twi_writeTo/readFrom`,
`spi_flash_read/write/erase_sector`, `system_restart`, watchdog and Wi-Fi mode
calls.  Anything else of the ROM that is called and not emulated stops the run
with its name.

- Time is counted in CPU cycles at 80 or 160 MHz with the costs that the
  [ESPboy-edu/espboy_emulator](https://github.com/ESPboy-edu/espboy_emulator)
  (MIT) measured on a console with warm loops: 1 per instruction, 2 for `MULL`,
  +2 per taken branch or call (`RET` +3), +1 when the next instruction uses a
  register a load has just filled, `MEMW` 4 after a store, +5 per load from
  IRAM, ROM or the mapped flash (literals included), +15 per peripheral
  register read and +10 per write (half in cycles at 80 MHz, bus time), none
  from DRAM.  Not validated here, and not in that reference: the cost of an
  instruction-cache miss (`GM0_ESP_FLASH_MISS=<cycles per 32-byte line>` turns
  on a 32 KiB direct-mapped model, off by default), SPI bytes at the clock the
  firmware programs, flash erase 45 ms and program 0.7 ms per page.  So these
  are estimates built on measured instruction costs, not measurements of this
  firmware on the chip.
- The display is the ST7735S datasheet model (132×162 18-bit memory, `MADCTL`
  mirroring, 12/16/18-bit pixels) with what the ESPboy's panel does:
  column and row addresses keep only their low byte, `SWRESET` keeps `MADCTL`
  and `COLMOD`, the panel is BGR (`MADCTL.BGR` clear swaps red and blue at scan
  time, memory already written included), white while asleep or off, and it
  listens only while MCP23017 B0 (chip select) is low.  With TFT_eSPI's setup
  (`MADCTL` 0xC8, window column 2, row 3, as m0boy's firmware) the glass pixel
  (x, y) is memory (129 − x, 128 − y).  Partial mode, scrolling and idle are not
  modelled.
- The firmware's flash saves (`0x3C0000-0x3FB000`) persist in `<name>.sav` next
  to the firmware (or the zip).
- Tests: `tests/esp/run_tests.py` compares the Xtensa core and the ROM
  functions with the host on C programs built for lx106 at several
  optimisation levels; `tests/test_esp.py` runs a small firmware through the
  whole machine (display, buttons, timer1 audio, flash saves).  Both need
  PlatformIO's `toolchain-xtensa` and are skipped without it.

### Controls

| | META | Pokitto |
|---|---|---|
| D-pad | arrows, ZQSD/WASD | arrows, IJKL |
| A / B | Space or J / Ctrl or K | Space or A / Ctrl, S or B |
| MENU / C | Enter or U | Enter, D or C |
| HOME / backlight | `*` or I (hold 3 s to reset the game) | F |

ESPboy (META keys): D-pad as the META, A = ACT (J), B = ESC (K), MENU (U) = right
auxiliary button (Start), HOME (I) = left auxiliary button (Select; hold for the
Game Boy frontend's menu).

| Key | Action |
|---|---|
| F5 | reset |
| F8 | display filter, cycled: pixel (default), Game Boy green (DMG), LCD RGB, scanlines, grid, none |
| F10 | scaling mode: integer, fitted, stretched |
| F11 | fullscreen |

Game controllers are supported through SDL_GameController with hot-plug, on
both targets.  The META button shift-register bit order follows the SPI speed
the game reads it at; `EMU_BTN_ORDER=lapinou` forces the homebrew order.

### Platform differences

| Feature | Native | WebAssembly |
|---|---|---|
| Audio | SDL2, locked to emulated time (±0.5 % rate regulation) | WebAudio, starts on first user gesture |
| Saves | `.SAV`/`.STA` written back to disk; Pokitto EEPROM persisted as `<game>.eeprom` | in memory, lost when the tab closes |
| Flash self-programming (loaders) | emulated | ignored |
| Game list | — | one-click dock (`wasm/games.js`), games can be embedded in the standalone file |
| Pause | — | dock buttons (pause, stop) |

## Accuracy

gm0 targets the hardware rather than other emulators: parity with the
original TypeScript emulator was dropped deliberately in October 2026.

- Instructions are charged their Cortex-M0+ cycle cost (ALU 1, load/store 2,
  taken branch 2, BL 3, LDM/STM/PUSH/POP 1+N, POP {pc} 3+N), plus a wait
  state on each miss of the 8-line NVM cache (instruction and data).
- The NVIC is the same ARMv6-M model on both targets: ISER/ICER enables,
  PRIMASK, strict-priority preemption over the four priority levels (IPR),
  the 8-word exception frame with STKALIGN, and the 15-cycle entry latency.
  Lines still asserted on exception return are pended again (TC and DMAC on
  the META, CT32B0/1 and pin interrupts on the Pokitto).
- On the Pokitto (LPC11U68, Cortex-M0+ r0p1), pin edges are latched in the
  NVIC when they happen, and SysTick fires on COUNTFLAG with TICKINT set.
- Peripheral registers are accessed by byte lane: an access of any width
  updates exactly the bytes it covers.
- TC4/TC5 run at the rate derived from their CTRLA prescaler and CC0; the
  SPI DMA to the display is clocked by the SERCOM baud rate while the CPU
  keeps running.
- The META display's color order follows the current MADCTL BGR bit
  (ST7735): BGR=1 until the game re-initializes the screen (the state the
  bootloader leaves), and native RGB565 after a final init with BGR=0
  (0x60, standard library and m0boy games).

## Performance

Measured on the development machine (x86-64, GCC 16, `-O3 -flto`), raw speed
without frame pacing, as a multiple of real time:

| Title | Target | Speed |
|---|---|---|
| lapinou | META | 4.4× |
| Celeste | META | 4.6× |
| Cats & Coins | META | 5.5× |
| converted Game Boy firmware (sml) | META | 5.0× |
| Pandemic | Pokitto | 4.1× |
| converted Game Boy firmware (sml) | ESPboy | 1.6× |

Numbers depend on the host and its load.  The WebAssembly build runs at full
speed in current browsers.

## Debugging

Diagnostics are enabled through environment variables; the most useful:

| Variable | Effect |
|---|---|
| `EMU_INPUT="frame:keys:len,..."` | scripted input (keys among `UDLRABMH`), e.g. `200:A:5,300:R:40` |
| `EMU_NOPACE=1` | run as fast as possible (benchmarks) |
| `EMU_TRACE=1` | screen hash every 60 frames, periodic CPU trace |
| `TRACE_TAIL=N` | keep the last N executed instructions, dumped on a wild PC |
| `EMU_PROF=file` | per-address cycle profile written at exit |
| `EMU_AUDIO_STATS=1` | audio producer/consumer statistics |
| `EMU_FIXED_RTC=t`, `ADC_FIXED=1` | deterministic RTC and ADC |
| `EMU_DEBUG=1`, `SD_DEBUG=1`, `NVM_DEBUG=1`, `EMU_DMA_DEBUG=1`, `EMU_LCD_DEBUG=1`, `EMU_SYSCTRL_DEBUG=1` | peripheral traces |
| `FAT_DUMP=file`, `FAT_DUMP_EXIT=file` | write the FAT image as built at mount, or the card at exit |
| `FLASH_DUMP=file` | write the META flash at exit (after any self-programming) |

## Tests

```sh
tests/unit/run.sh                                        # core unit tests (flash programming)
GM0_GAMES=dir tests/parity/run.sh build/bin/gm0          # bit-exact parity on local games
```

`tests/parity/run.sh` runs each game listed in `tests/parity/games.txt`
(1500 frames, scripted input) and compares three SHA-256 fingerprints with
`tests/parity/refs.txt`: the screen (`EMU_TRACE` lines), the `--wav`
recording and the `--shot` capture.  The games are not in the repository
(`GM0_GAMES` defaults to `wasm/`).  Each game runs alone in a throwaway
directory, with `--eeprom-ro`, so no saved EEPROM is inherited.  Rewrite the
references (`-u`) only after a change that is meant to alter the output.

## Known limitations

- Windows requires MSYS2 MinGW-w64 (`dirent.h`); MSVC is untested.
- In WebAssembly, flash writes are ignored and saves do not survive the
  session.
- Cards are built as FAT16 (superfloppy or MBR depending on the target).
- Pokitto: the I/O port is not single-cycle (loads and stores are charged
  as ordinary bus accesses), pin interrupts are edge-only (the level mode
  and the pattern match engine are not modelled), and the USART, ADC and SCT
  interrupts are not connected.
- Pokitto: the IRQ numbers (pins 0-7, CT32B0/1 as IRQ 18/19) come from the
  PokittoEmu port. The LPC11U6x datasheet (rev. 1.5) does not list them;
  the user manual (UM10732) does and has not been checked against them.

Planned and investigated work is tracked in [TODO.md](TODO.md).

## Development

gm0 was written entirely by AI coding agents; no C code was typed by hand.
The first version — about 5,800 lines of C and 66 commits, from September 30
to October 6, 2026 — was produced by GLM-5.3-Flash through the ZCode agent.
Statistics for that week, from the session database:

- 14 sessions, 122 prompts;
- 5,446 model requests, 5,488 tool calls;
- 1.84 billion tokens processed (1.82 billion cache reads, about 4.2 million
  generated), about 52 hours of cumulative model time.

Later refactoring, hardware-accuracy fixes and performance work were
co-authored with Claude Opus 5.5 in Claude Code.

## Credits and references

Logic was ported from two MIT-licensed emulators:

- **gamebuino-emulator** by Andy O'Neill (TypeScript) — the first C port
  reproduced it tick for tick:
  [aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator);
- **PokittoEmu** by Felipe Manga — the LPC11U6x/Cortex-M0+ side is a C port
  of it: [felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu).

Everything else follows the vendor documentation:

- Microchip SAM D21/DA1 datasheet, DS40001882 —
  <https://www.microchip.com/en-us/product/ATSAMD21G18>;
- NXP LPC11U6x datasheet and user manual UM10732 —
  <https://www.nxp.com/docs/en/data-sheet/LPC11U6X.pdf>;
- Sitronix ST7735 controller datasheet;
- ARMv6-M Architecture Reference Manual —
  <https://developer.arm.com/documentation/ddi0419/latest>.

Hardware behaviour was also established by analysing real software, without
reusing any of its code: the lapinou source (bare-metal display, SD and audio
drivers, which exposed the display DMA/SPI protocol, TC4 clocking and the
chip-select pull-ups), META binaries, and the official
[Gamebuino META library](https://github.com/Gamebuino/Gamebuino-META)
(SdFat stack, DMAC descriptors, TC5 audio), stepped through in the emulator.

Ecosystems: [gamebuino.com](https://gamebuino.com) for the META,
[PokittoLib](https://github.com/pokitto/PokittoLib) for the Pokitto.

## License

gm0 is released under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
(© 2026 r043v): attribution, non-commercial, share-alike.

Logic derived from the two MIT-licensed emulators remains under MIT; their
notices are reproduced in [LICENSE](LICENSE):
[aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(Andy O'Neill, 2017) and
[felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu)
(Felipe Manga, 2017).  Implementing behaviour described in vendor
datasheets (Microchip, NXP, Sitronix, Arm) does not create a derivative of
those documents, and no code was taken from the lapinou source, META
binaries, or the official library.
