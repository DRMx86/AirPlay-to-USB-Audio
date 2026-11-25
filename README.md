# I2S-to-USB-Audio

An ESP32-S3 firmware that takes a stereo **I2S** audio stream in and presents it to a
computer or USB amplifier as a **USB Audio Class** input device (a "USB microphone").

```
 I2S source ──BCK/WS/DATA──▶ ESP32-S3 ──USB (UAC, 48 kHz 16-bit stereo)──▶ PC / USB amp
```

This is a derivative of [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by
[rbouteiller](https://github.com/rbouteiller). The AirPlay 2, Bluetooth, WiFi/Ethernet,
web UI, display and button code has been removed from this branch; only the USB audio
output and the resampler were kept. See [LICENSE](LICENSE) — **non-commercial use only**.

## Hardware

- An ESP32-S3 board with a native USB port (GPIO19 = D−, GPIO20 = D+). ESP32-S2 and
  ESP32-P4 also have USB-OTG but are untested here. The classic ESP32 has no USB device
  mode and is not supported.
- An I2S source: an ADC (PCM1808, ...), an S/PDIF-to-I2S receiver, a Raspberry Pi,
  another microcontroller, etc.

Default wiring (change in `menuconfig`):

| Signal | ESP32-S3 GPIO | Direction (slave mode) |
| --- | --- | --- |
| BCK (bit clock) | 4 | in |
| WS / LRCK (word select) | 5 | in |
| DIN (serial data) | 6 | in |
| MCLK | – (optional) | out, master mode only |
| GND | GND | common ground is required |

Connect the S3's native USB port to the host. If your board has a separate UART USB
port, use that one for flashing and logs.

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
idf.py -p <port> flash monitor
```

If you have built an earlier (AirPlay) version of this repository in the same checkout,
delete the old `sdkconfig` and `managed_components/` first so the new configuration
and dependency set are generated cleanly.

## Configuration

`idf.py menuconfig` → **I2S to USB Audio**:

| Option | Default | Notes |
| --- | --- | --- |
| I2S role | Slave | Slave: the source supplies BCK/WS. Master: the ESP32 supplies BCK/WS (and MCLK). |
| Sample rate | 48 kHz | 44.1 kHz input is resampled to 48 kHz (uses more CPU). |
| Slot width | 32-bit | Most ADCs/receivers use 32-bit slots. Audio is truncated to 16 bits. |
| Format | Philips | Or MSB / left-justified. |
| BCK / WS / DIN / MCLK GPIO | 4 / 5 / 6 / -1 | |
| USB ring buffer | 60 ms | Absorbs jitter and clock drift. |
| USB prefill | 20 ms | Audio buffered before streaming starts; sets the steady-state latency. |

The USB side is fixed at 48 kHz, 16-bit, stereo (`CONFIG_UAC_SAMPLE_RATE` must equal
`CONFIG_OUTPUT_SAMPLE_RATE_HZ`).

## How it works

- `main/audio/audio_input_i2s.c` — I2S RX channel plus a capture task. Reads 5 ms blocks,
  converts to 16-bit, optionally resamples 44.1 → 48 kHz and pushes the result to the USB output.
- `main/audio/audio_output_usb.c` — USB UAC device (via `espressif/usb_device_uac`) and a
  ring buffer. The host pulls audio from the buffer in the UAC input callback.
- `main/audio/audio_resample.c` — sinc resampler wrapper (`components/audio-resampler`).

The serial log prints a status line every 5 seconds: whether I2S data is arriving, frames
captured, USB buffer fill, and overrun/underrun counts.

### Clock drift

The I2S source and the USB host run from different clocks, so the buffer slowly fills or
drains. When it is full the excess frames are dropped (an *overrun*); when it runs dry the
host receives silence until the buffer refills to the prefill level (an *underrun*). With
typical crystals this is a rare, brief glitch. If you hear regular clicks, make the I2S
source and the USB host share a clock domain where possible, or enlarge the buffer. There
is no asynchronous sample-rate conversion or UAC feedback endpoint yet.

## Status

This branch has not been run on hardware yet. The sources have been syntax-checked against
stubs of the ESP-IDF APIs, but a full ESP-IDF build and an end-to-end listening test are
still to do.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).
Provided as-is without warranty.
