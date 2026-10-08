# gm0

**A cycle-counted emulator for the Gamebuino META (SAMD21, Cortex-M0+) and
the Pokitto (LPC11U6x, Cortex-M0), written in C with an SDL2 frontend and a
WebAssembly build.**

> Version française : [README.fr.md](README.fr.md)

gm0 runs unmodified firmware for both consoles from a single binary.  The
target is detected from the firmware image (initial stack pointer in SAMD21
or LPC SRAM) and can be forced with `--target`.  The same C core is compiled
natively and to WebAssembly.

- **CPU** — ARMv6-M Thumb interpreter, cycle-counted per the Cortex-M0+
  timing tables, including NVM cache misses on the META.
- **META peripherals** — PORT, SERCOM4/5 (ST7735 display, SPI SD card,
  button shift register), DMAC (triggered channels, chained descriptors,
  suspend/resume), TC4/TC5, DAC, SysTick, NVIC with real priorities and
  exception stacking, minimal NVMCTRL.
- **Pokitto peripherals** — SYSCON/PLL, CT32B, SCT, SSP, GPIO, IAP and ROM
  API, persistent 4 KB EEPROM, native `.pop` container support.
- **Storage** — SPI SD card backed by a raw image, a `.zip`, or a host folder
  converted to FAT16 on the fly; saves written by games are written back.

## Status

| Target | State | Tested titles |
|---|---|---|
| Gamebuino META | Playable | lapinou (bare-metal drivers), Celeste, Cats & Coins, Picomon, Yatzy, Reuben Quest, site loaders, converted Game Boy firmwares (DMA/SPI protocols 0.4.0+) |
| Pokitto | Playable | Pandemic, Galaxy Fighters (`.pop` with music streamed from the card) |
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
make wasm                    # produces wasm/gm0-standalone.html
```

The output is a single file with the wasm module embedded; it can be opened
directly from `file://`.  A hosted build is available at
<https://r043v.github.io/gm0/> (emulator only — load your own games).

## Usage

```sh
gm0 [firmware] [card] [options]
```

| Argument | Meaning |
|---|---|
| `firmware` | `.bin`, or `.pop` (Pokitto) |
| `card` | `.img` raw image, `.zip` (full card), or a folder (FAT16 built on the fly). On the META the card defaults to the firmware's folder. |
| `--target meta\|pokitto` | force the target |
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

### Controls

| | META | Pokitto |
|---|---|---|
| D-pad | arrows, ZQSD/WASD | arrows, IJKL |
| A / B | Space or J / Ctrl or K | Space or A / Ctrl, S or B |
| MENU / C | Enter or U | Enter, D or C |
| HOME / backlight | `*` or I (hold 3 s to reset the game) | F |

| Key | Action |
|---|---|
| F5 | reset |
| F8 | display filter, cycled: none, pixel grid, Game Boy green (DMG), LCD RGB, scanlines |
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
- The NVIC implements PRIMASK, strict-priority preemption, the 8-word
  exception frame with STKALIGN, and level-triggered lines that are
  re-evaluated on exception return.
- Peripheral registers are accessed by byte lane: an access of any width
  updates exactly the bytes it covers.
- TC4/TC5 run at the rate derived from their CTRLA prescaler and CC0; the
  SPI DMA to the display is clocked by the SERCOM baud rate while the CPU
  keeps running.

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
| `EMU_DEBUG=1`, `SD_DEBUG=1`, `NVM_DEBUG=1`, `EMU_DMA_DEBUG=1`, `EMU_LCD_DEBUG=1` | peripheral traces |
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
- **PokittoEmu** by Felipe Manga — the LPC11U6x/Cortex-M0 side is a C port
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
