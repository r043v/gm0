# Gamebuino AKA (experimental)

The AKA machine executes ESP32-S3 Xtensa firmware code using the shared
Xtensa decoder, with windowed call4/8/12 and the scalar FP instructions
exercised by the tests. CPU cycles and transfer delays are estimates. The current host model uses
a fixed 240 MHz timebase; lower CPU choices are not performance models.
The register-window spill/fill service is handled by the emulator rather
than by executing the ROM's window exception handlers.

This is an application-level machine: it loads the application's ELF or
ESP image, initialises its RAM and enters `m0boy_console_main` or `app_main`.
It does not boot the ESP-IDF bootloader, run FreeRTOS or emulate the entire
ESP32-S3 peripheral register map. Supported platform calls are intercepted;
unknown instructions and unknown services stop with a diagnostic and a
nonzero CLI exit status.

## Use

```
cmake -G Ninja -S . -B build-aka -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
ninja -C build-aka
build-aka/bin/gm0 /path/to/firmware.elf
build-aka/bin/gm0 /path/to/Game.zip --sym /path/to/firmware.sym
```

The ELF must contain symbols. For `.bin` or `.zip`, keep `firmware.sym`
beside/in the archive, or pass `--sym`. Generate it from the **matching** ELF:
`xtensa-esp32s3-elf-nm -n firmware.elf > firmware.sym`. A symbol table from
another build is not suitable. Images are detected using the ESP32-S3 chip
identifier (9); their layout is checked before copying segments.
Without `m0boy_console_main`/`app_main` in the symbol table (or with no
symbol table at all), the loader falls back to the image/ELF entrypoint in a
**degraded mode**: the platform services cannot be hooked by name, so the run
stops at the first unhooked service — for an ESP-IDF image that is typically
the ROM startup code. A warning is printed at load and a hint repeats on the
first fault.

The degraded boot goes through the ESP32-S3 ROM startup thanks to
fixed-address HLE of the routines `call_start_cpu0` calls (`rtc_get_reset_reason`,
`memset`/`memcpy`/`memmove`/`bzero`, the cache/MMU family) and a read-as-zero,
write-ignored peripheral window at 0x60000000. That is enough to reach the
application; display/input stay dark since nothing hooks the platform calls.
To actually run BunnyMark, build it from source so symbols exist:

```
git clone -b v5.5.1 --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git
git clone https://github.com/joyrider3774/bunnymark_ports.git
git clone https://github.com/jmp42/Gamebuino_AKA_lib.git
IDF_PATH=<esp-idf> ./esp-idf/install.sh esp32s3 && . <esp-idf>/export.sh
AKA_LIB_DIR=<Gamebuino_AKA_lib> idf.py -B build build
xtensa-esp32s3-elf-nm -n build/bunnymark.elf > build/firmware.sym
gm0 build/bunnymark.bin --sym build/firmware.sym
```

The current Gamebuino_AKA_lib drives the LCD/SD/inputs itself; the services
gm0 hooks (`gb_graphics::update` reading `framebuffer`, `gb_ll_expander_read`,
`xTaskGetTickCount`, `malloc`...) cover the whole BunnyMark path, so the game
renders, counts and answers the buttons.

The m0boy platform boundary provides clock, scaled RGB565 rows, the shared
buttons and direct SD sectors. Its audio generation runs as guest code;
the emulator drains the PCM ring into the common SDL/WAV output. The SD
backing image uses the same frontend as the other machines; `--out-img`
exports writes. `EMU_INPUT`, `--shot`, `--wav`, F5 and the SDL window work.
CPU snapshots and ARM cycle profiles are not available for AKA.

A small set of Gamebuino AKA library calls also has HLE services (core init,
joystick poll, expander input, refresh and display update). The library's
software drawing and the game's own code execute on Xtensa. This is enough
for the supplied BunnyMark firmware with its reconstructed symbol table;
it is not a claim that every library/SDK call is supported. Its source:
https://github.com/joyrider3774/bunnymark_ports/tree/main/aka .
A raw BunnyMark release ZIP without symbols is rejected with a diagnostic.
No game binary, image asset or recovered game symbol map is committed here.

The LCD/input/audio/storage wiring for m0boy's real AKA target comes from
https://github.com/jmp42/Gamebuino_AKA_lib (CORE v1.4). HLE checks do not
validate the electrical drivers. Physical panel colours/orientation,
I2S/codec clocking, SDMMC and ADC inputs still need device checks.

## Tests

`tests/aka/test_cpu.py` compares deeply nested windowed calls, scalar FP
arithmetic, comparisons and conversions against the host at O0/O1/O2/Os/O3.
`tests/aka/test_machine.py` compiles a native firmware and checks row scaling,
RGB565, input changes, SD read/write, PCM and rejection of a truncated ELF.
Both need the ESP32-S3 GCC toolchain (PlatformIO default path or `AKA_GCC`).
The original ESP8266 tests accept `PLATFORMIO_CORE_DIR` for packages installed
outside `~/.platformio`.
