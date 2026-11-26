# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

I2S-to-USB-DAC — firmware that takes an I2S input (e.g. from a Bluetooth audio board) and plays it on a USB DAC dongle. The ESP32-S3 is the USB **host** (native USB port); UAC 1.0 and UAC 2.0 dongles are supported. Derived from the airplay-esp32 project; on `main` the source is AirPlay, here it is I2S, and AirPlay, Bluetooth, WiFi/Ethernet, web UI, displays and buttons were removed.

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
├── main.c                    # app_main: board init, USB host output, I2S input, 5 s status log
├── Kconfig.projbuild         # "I2S to USB DAC Audio" menu (I2S role/rate/pins, USB DAC rate, FIFO sizes, VBUS GPIO)
└── audio/
    ├── audio_input_i2s.c/.h  # I2S RX channel + capture task (slave/master, 16/32-bit slots)
    ├── audio_output_usb_host.c # native USB host UAC 1.0/2.0 streamer (descriptor parse, clock/rate setup, iso OUT)
    ├── audio_output.h        # output API: init / write / flush / stats
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

- **Data path**: I2S RX (DMA) -> capture task (`i2s_capture`, core 1, prio 7) -> `audio_output_write()` -> stream-buffer FIFO -> isochronous OUT transfer callbacks (USB client task, core 0) -> USB DAC dongle.
- **Format**: the FIFO is always 16-bit stereo at `CONFIG_OUTPUT_SAMPLE_RATE_HZ`; it is expanded to 24/32-bit in the iso callback if the dongle only offers wider slots. 32-bit I2S slots are truncated to the top 16 bits. Only 44.1 -> 48 kHz resampling exists, so I2S rate and USB rate must be equal or 44.1 in / 48 out.
- **Drift handling**: `audio_output_write()` never blocks; when the FIFO is full the excess frames are dropped (overrun). On underrun the iso callback sends silence and waits for `CONFIG_USB_AUDIO_PRIME_MS` of audio before resuming.
- **USB host**: ESP32-S3 USB is full-speed only. The native USB port is taken over by the host, so flash/monitor via the UART port. UAC 2.0: rate set on every Clock Source entity before selecting the alt; UAC 1.0: rate set on the endpoint after SET_INTERFACE. `usb_host_interface_claim()` does not send SET_INTERFACE; the explicit control transfer is required.
- **Host-side tests**: the descriptor parser and packet sizing can be tested on a PC by `#include`-ing `audio_output_usb_host.c` into a test with stubbed FreeRTOS/USB calls (real `usb/*.h` headers from ESP-IDF).
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
