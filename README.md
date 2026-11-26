# I2S-to-USB-DAC

An ESP32-S3 firmware that takes a stereo **I2S** audio stream in (for example from a
Bluetooth audio board) and plays it through a **USB DAC dongle** plugged into the
ESP32-S3's native USB port. The ESP32-S3 acts as the USB **host**; **UAC 1.0 and
UAC 2.0** dongles are supported.

```
 Bluetooth board ──I2S (BCK/WS/DATA)──▶ ESP32-S3 ──USB host──▶ USB DAC dongle ──▶ speakers
```

This is a derivative of [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by
[rbouteiller](https://github.com/rbouteiller). On the `main` branch the audio source is
AirPlay; on this branch it is I2S, and the AirPlay, Bluetooth, WiFi/Ethernet, web UI, display
and button code has been removed. The USB host backend comes from `main`. See
[LICENSE](LICENSE) — **non-commercial use only**.

## Hardware

- An ESP32-S3 board with **two USB ports**: one USB-UART port (for flashing and logs) and
  one **native USB** port (GPIO19 = D−, GPIO20 = D+) for the DAC dongle.
- A USB DAC dongle, **UAC 1.0 or UAC 2.0**, with stereo PCM output and **full-speed** operation
  (the ESP32-S3 USB is full-speed only, 12 Mbps). Cheap UAC 1.0 dongles and Apple's USB-C
  adapter are fine; DACs that only work at high-speed UAC 2.0 will not enumerate.
- **5 V on the native USB port's VBUS.** The dongle must be powered. If your board does not
  supply 5 V there, power it externally, or switch it with a GPIO via
  `CONFIG_USB_HOST_VBUS_EN_GPIO`.
- An I2S source (the Bluetooth board, an ADC, an S/PDIF receiver, ...).

Default I2S wiring (change in `menuconfig`):

| Signal | ESP32-S3 GPIO | Direction (slave mode) |
| --- | --- | --- |
| BCK (bit clock) | 4 | in |
| WS / LRCK (word select) | 5 | in |
| DIN (serial data) | 6 | in |
| MCLK | – (optional) | out, master mode only |
| GND | GND | common ground is required |

The native USB port is taken over by the USB host, so **flash and monitor through the
USB-UART port**.

## Build and flash

PlatformIO:

```bash
git clone --recursive https://github.com/DRMx86/AirPlay-to-USB-Audio
cd AirPlay-to-USB-Audio
git checkout i2s-to-USB-Audio
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t monitor
```

ESP-IDF (>= 5.5):

```bash
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
idf.py -p <uart-port> flash monitor
```

If you built an earlier version of this repository in the same checkout, delete the old
`sdkconfig` and `managed_components/` first (and `dependencies.lock` if the component
manager complains), so the configuration and dependencies are generated cleanly.

## Configuration

`idf.py menuconfig` → **I2S to USB DAC Audio**:

| Option | Default | Notes |
| --- | --- | --- |
| I2S role | Slave | Slave: the source supplies BCK/WS. Master: the ESP32 supplies BCK/WS (and MCLK). |
| I2S sample rate | 48 kHz | **Must match what the source sends.** 44.1 kHz input can be converted to 48 kHz. |
| Slot width | 32-bit | Most sources use 32-bit slots. Audio is truncated to 16 bits internally. |
| Format | Philips | Or MSB / left-justified. |
| BCK / WS / DIN / MCLK GPIO | 4 / 5 / 6 / -1 | |
| USB DAC sample rate | 48 kHz | Rate sent to the dongle (44100 and 48000 work on nearly all). |
| USB FIFO size | 60 ms | Absorbs jitter and clock drift. |
| USB prefill | 20 ms | Audio buffered before playback starts; sets the steady-state latency. |
| VBUS enable GPIO | -1 | GPIO that switches 5 V onto the host port, if your board has one. |

Only 44.1 → 48 kHz conversion is built in. Set the I2S rate and the USB DAC rate equal (no
conversion), or use 44.1 kHz in and 48 kHz out. Pick the rate your Bluetooth board really
sends over I2S.

## How it works

- `main/audio/audio_input_i2s.c` — I2S RX channel plus a capture task. Reads 5 ms blocks,
  converts to 16-bit, optionally resamples 44.1 → 48 kHz and writes to the USB output.
- `main/audio/audio_output_usb_host.c` — USB host backend using the ESP-IDF USB Host library
  directly. On connect it reads the descriptors, finds a stereo PCM isochronous-OUT
  alt-setting (16-bit preferred, 24/32-bit if that is all the dongle offers), sets the sample
  rate (UAC 2.0: on the clock sources; UAC 1.0: on the endpoint), selects the interface,
  unmutes the feature units and streams through isochronous transfers. Hot-plug is supported.
- `main/audio/audio_resample.c` — sinc resampler wrapper (`components/audio-resampler`).

The serial log prints a status line every 5 seconds: whether I2S data is arriving, frames
captured, whether a USB DAC is connected, FIFO fill, and overrun/underrun counts. When a
dongle is plugged in, the log also lists its audio alt-settings and the UAC version found,
which is useful if a particular dongle does not work.

### Clock drift

The I2S source and the USB DAC run from different clocks, so the FIFO slowly fills or drains.
When it is full the excess frames are dropped (an *overrun*); when it runs dry the dongle gets
silence until the FIFO refills to the prefill level (an *underrun*). That is a rare, brief click
with typical crystals. There is no asynchronous sample-rate conversion or feedback-endpoint
support yet.

## Status

Not yet run on hardware. The USB host code is a port of the backend proven on `main`; it
type-checks against the real ESP-IDF USB headers, and the descriptor parser, packet sizing and
24-bit expansion pass host-side tests with synthetic UAC 1.0 / 2.0 descriptors. A full ESP-IDF
build and a listening test with your dongle are still to do.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).
Provided as-is without warranty.
