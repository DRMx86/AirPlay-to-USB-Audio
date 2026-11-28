# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Web Radio to USB DAC — firmware that streams an Icecast web radio (MP3 or AAC, HTTP or HTTPS) over Wi-Fi and plays it on a USB DAC dongle. The ESP32-S3 is the USB **host** (native USB port); UAC 1.0 and UAC 2.0 dongles are supported. Derived from the airplay-esp32 project; on `main` the source is AirPlay, here it is a web radio stream, and AirPlay, Bluetooth, Ethernet, web UI, displays and buttons were removed.

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
├── main.c                    # app_main: board init, USB host output, Wi-Fi manager, radio player, 5 s status log
├── Kconfig.projbuild         # "Web Radio to USB DAC Audio" menu (setup AP, stream buffers, button, USB DAC rate, FIFO sizes, initial volume, VBUS GPIO)
├── idf_component.yml         # espressif/esp_audio_codec (MP3/AAC decoders)
├── net/
│   ├── wifi_manager.c/.h     # saved network -> else setup AP "WebRadio" + captive portal; NVS creds; HTTP API (/api/status, networks, connect, forget, volume)
│   ├── captive_dns.c/.h      # answers every A query with the AP address (captive portal)
│   └── portal.html           # the single-file dark setup/volume page, embedded via EMBED_FILES
├── radio/
│   ├── stations.c/.h         # station table (add stations here; first entry plays at boot)
│   └── radio_player.c/.h     # net task (HTTP(S) + ICY metadata) -> byte buffer -> decoder task -> USB output
└── audio/
    ├── audio_output_usb_host.c # native USB host UAC 1.0/2.0 streamer (descriptor parse, clock/rate setup, iso OUT)
    ├── audio_output.h        # output API: init / write / flush / stats
    └── audio_resample.c/.h   # sinc resampler wrapper (stream rate -> USB rate)

components/
├── boards/                   # Board support (esp32-generic, esp32s2/s3-generic, waveshare-esp32p4)
├── audio-resampler/          # sinc-based audio resampler
├── dac*/                     # DAC drivers, unused by this firmware (kept from the upstream project)
├── display/, u8g2*/          # unused; only the stub is built (display Kconfig menu was removed)
├── spiffs_storage/           # unused
└── board_utils/
```

## Key Conventions

- **Data path**: `net_task` (core 0, prio 5) reads the HTTP body, strips ICY metadata and pushes bytes into a PSRAM stream buffer -> `dec_task` (core 1, prio 6) decodes with the `esp_audio_codec` simple decoder -> mono->stereo and resample if needed -> `audio_output_write()` -> stream-buffer FIFO -> isochronous OUT transfer callbacks (USB client task, core 0) -> USB DAC dongle.
- **Format**: the USB FIFO is always 16-bit stereo at `CONFIG_OUTPUT_SAMPLE_RATE_HZ`; it is expanded to 24/32-bit in the iso callback if the dongle only offers wider slots. Keep the output rate at 48000: the resampler wrapper is compiled out when the output is 44100, so the player refuses other stream rates in that case.
- **Backpressure**: `audio_output_write()` never blocks and drops on overflow, so `dec_task` checks the FIFO free space (`audio_output_get_stats`) and waits. That stalls the byte buffer and TCP, so the server is read at playback speed. With no DAC connected the audio is discarded at real-time speed.
- **Station / stream changes**: `radio_player_select()` bumps `s_select_gen`; the net task then calls `flush_decoder()` (bumps `s_gen`, waits for `s_dec_gen` ack) so the decoder drops its state, buffered bytes and queued USB audio. A plain reconnect to the same station does not flush; the decoder's parser resyncs by itself.
- **Wi-Fi manager**: credentials live in NVS namespace `wifi` (not in Kconfig/sdkconfig; the old `RADIO_WIFI_*` options are gone). `s_sta_wanted` says whether the station should keep (re)connecting; the portal sets it false and disconnects so scans work. A boot with a saved network that fails `CONFIG_WIFI_MAX_RETRY` times opens the portal; once it has connected, drops are retried forever. A new network from the page is only saved after it gets an IP (`s_trying_new`), and a failed attempt falls back to the old one; the page follows progress by polling `/api/status` (`attempt`/`result`). The HTTP server stays up on the home network. No authentication.
- **Volume**: starts at `CONFIG_USB_AUDIO_INITIAL_VOLUME_PERCENT` and is applied in `setup_device()` before streaming starts; never set Feature Units to 0 dB (that is the maximum). `audio_output_set_volume_percent()` maps 0-100 % onto the top 60 dB of each Feature Unit's range (UAC1: GET_MIN/MAX/RES, UAC2: RANGE; master channel, else channels 1+2), mutes at 0, and falls back to a digital gain in `audio_output_write()` when no Feature Unit has a volume. Control transfers are serialised by `s_ctrl_lock` (HTTP task and USB setup task share EP0).
- **Reconnects**: failures back off 1 s -> 10 s; after three consecutive read timeouts (`CONFIG_RADIO_HTTP_TIMEOUT_MS` each) the stream is reopened.
- **Stream support**: MP3 and AAC (ADTS) only, chosen from `Content-Type` (missing type defaults to MP3). Ogg and old Shoutcast v1 (`ICY 200 OK`) are unsupported.
- **USB host**: ESP32-S3 USB is full-speed only. The native USB port is taken over by the host, so flash/monitor via the UART port. UAC 2.0: rate set on every Clock Source entity before selecting the alt; UAC 1.0: rate set on the endpoint after SET_INTERFACE. `usb_host_interface_claim()` does not send SET_INTERFACE; the explicit control transfer is required.
- **Committed sdkconfig**: `sdkconfig` is tracked (despite `.gitignore`), uses CRLF line endings and overrides `sdkconfig.defaults*`. When changing defaults, change it too and keep its line endings. Keep it free of secrets: Wi-Fi credentials are entered on the device.
- **Host-side tests**: the descriptor parser, packet sizing and volume code can be tested on a PC by `#include`-ing `audio_output_usb_host.c` into a test with stubbed FreeRTOS/USB calls (real `usb/*.h` headers from the esp-usb repo, `host/usb/include`). The same trick works for `radio_player.c` (ICY splitting, content-type mapping) and `wifi_manager.c` (form parsing, JSON escaping); build `captive_dns.c` as a separate object (both define `s_ip`). Link with `-no-pie -Wl,--unresolved-symbols=ignore-all` so untouched ESP-IDF calls need no fakes.
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
