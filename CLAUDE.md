# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

I2S-to-USB-Audio — firmware that turns an ESP32-S3 into a USB Audio Class (UAC) input device fed from an I2S input. Derived from the airplay-esp32 project; AirPlay, Bluetooth, WiFi/Ethernet, web UI, displays and buttons were removed on this branch.

## Build & Flash

**PlatformIO**:
```bash
pio run -e esp32s3 -t build         # Build firmware
pio run -e esp32s3 -t upload        # Build + flash
pio run -e esp32s3 -t monitor       # Serial monitor (115200 baud)
pio run -e esp32s3 -t menuconfig    # Kconfig configuration
```

**ESP-IDF** (native, >= 5.5):
```bash
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
idf.py -p /dev/ttyUSB0 flash monitor
```

Build environments: `esp32s3` (default) and `esp32s3-jtag`. Sdkconfig defaults are layered via `cmake_extra_args` (left-to-right override). Custom config: create `user_platformio.ini` to extend an environment.

## Architecture

```
main/
├── main.c                    # app_main: board init, USB output, I2S input, 5 s status log
├── Kconfig.projbuild         # "I2S to USB Audio" menu (I2S role/rate/pins, USB buffer sizes)
└── audio/
    ├── audio_input_i2s.c/.h  # I2S RX channel + capture task (slave/master, 16/32-bit slots)
    ├── audio_output_usb.c    # UAC device (espressif/usb_device_uac) + ring buffer
    ├── audio_output.h        # USB output API: init / write / flush / stats
    └── audio_resample.c/.h   # sinc resampler wrapper (44.1 -> 48 kHz when configured)

components/
├── boards/                   # Board support (esp32-generic, esp32s2/s3-generic, waveshare-esp32p4)
├── audio-resampler/          # sinc-based audio resampler
├── dac*/                     # DAC drivers, unused by this firmware (kept from the upstream project)
├── display/, u8g2*/          # unused; only the stub is built (display Kconfig menu was removed)
├── spiffs_storage/           # unused
└── board_utils/
```

## Key Conventions

- **Data path**: I2S RX (DMA) -> capture task (`i2s_capture`, core 1, prio 7) -> `audio_output_write()` -> byte ring buffer -> `usb_input_cb()` (called by the UAC stack) -> USB host.
- **Format**: the USB stream is always 16-bit stereo at `CONFIG_OUTPUT_SAMPLE_RATE_HZ` (must equal `CONFIG_UAC_SAMPLE_RATE`). 32-bit I2S slots are truncated to the top 16 bits.
- **Drift handling**: `audio_output_write()` never blocks; when the ring buffer is full the excess frames are dropped (overrun). On underrun the USB callback sends silence and waits for `CONFIG_USB_AUDIO_PRIME_MS` of audio before resuming.
- **Component structure**: Each component has its own `CMakeLists.txt` with `idf_component_register()`.
- **Git submodules**: `u8g2` and `u8g2-hal-esp-idf` are still declared as submodules — clone with `--recursive`.
- **Project name**: the CMake project is still called `airplay2-receiver` so existing local scripts that reference `build/airplay2-receiver.bin` keep working.

## Code Quality

**Requirements**: ESP-IDF >= 5.5 (tested against v5.5.2). Older versions may need workarounds.

**Formatting**: LLVM-style, 2-space indent, 80-char column limit. See `.clang-format`.

**Linting**: clang-tidy with bugprone, performance, portability, and readability checks. See `.clang-tidy`.

**Pre-commit hook**: auto-formats staged C/H files with clang-format, runs clang-tidy (requires `build/compile_commands.json`). Install via `git config core.hooksPath .githooks`.

**CI** (`.github/workflows/ci-release.yml`): On push/PR to `main`:
- `format-check`: clang-format dry-run on all C/H files (excludes `components/u8g2`)
- `lint-check`: clang-tidy on build output (requires ESP-IDF v5.5 toolchain)
- `build`: compiles the esp32s3 target config
- `release`: auto-creates GitHub release with merged firmware bins (only on push)

**Local tooling** (in `scripts/`):
```bash
scripts/format.sh          # Format all C/H files (excludes u8g2 submodule)
scripts/lint.sh            # Run clang-tidy on all C/H files
scripts/lint.sh --fix      # Attempt to auto-fix clang-tidy issues
```

**No unit tests**: This is embedded firmware — no test framework is in place. Manual testing on hardware is required.
