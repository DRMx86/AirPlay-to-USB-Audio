# Web Radio to USB DAC

An ESP32-S3 firmware that streams an **Icecast web radio** over Wi-Fi, decodes it (MP3 or
AAC) and plays it through a **USB DAC dongle** plugged into the ESP32-S3's native USB port.
The ESP32-S3 acts as the USB **host**; **UAC 1.0 and UAC 2.0** dongles are supported.

```
 Wi-Fi ──▶ Icecast stream (HTTP/HTTPS) ──▶ ESP32-S3 (decode) ──USB host──▶ USB DAC dongle ──▶ speakers
```

This is a derivative of [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) by
[rbouteiller](https://github.com/rbouteiller). On the `main` branch the audio source is
AirPlay; on this branch it is a web radio stream, and the AirPlay, Bluetooth, Ethernet, web
UI and display code has been removed. The USB host backend comes from `main`. See
[LICENSE](LICENSE) — **non-commercial use only**.

## Hardware

- An ESP32-S3 board with **PSRAM** and **two USB ports**: one USB-UART port (for flashing and
  logs) and one **native USB** port (GPIO19 = D−, GPIO20 = D+) for the DAC dongle.
- A USB DAC dongle, **UAC 1.0 or UAC 2.0**, with stereo PCM output and **full-speed** operation
  (the ESP32-S3 USB is full-speed only, 12 Mbps). Cheap UAC 1.0 dongles and Apple's USB-C
  adapter are fine; DACs that only work at high-speed UAC 2.0 will not enumerate.
- **5 V on the native USB port's VBUS.** The dongle must be powered. If your board does not
  supply 5 V there, power it externally, or switch it with a GPIO via
  `CONFIG_USB_HOST_VBUS_EN_GPIO`.
- A Wi-Fi network (2.4 GHz) with internet access.
- Optional: a push button between a GPIO and GND to switch stations (`menuconfig`).

The native USB port is taken over by the USB host, so **flash and monitor through the
USB-UART port**.

## Stations

Stations live in `main/radio/stations.c`. The first entry plays at boot:

```c
const radio_station_t g_stations[] = {
    {"Monster RX", "https://in-icecast.eradioportal.com:8443/monsterrrx"},
    /* add more here */
};
```

To add a station, append `{"Name", "http(s)://host:port/mount"},`. Streams must be MP3 or AAC
(ADTS) from an Icecast server. HTTPS works (certificates are checked against the built-in
Mozilla bundle), redirects are followed, and the stream is auto-detected from its
`Content-Type`. Ogg/Opus/Vorbis streams are not supported. Old Shoutcast v1 servers that
answer `ICY 200 OK` instead of `HTTP/1.x 200 OK` are not supported either.

With a button configured, each press switches to the next station. In code, call
`radio_player_select(index)` or `radio_player_next()`.

## Build and flash

**Set your Wi-Fi credentials first** (see Configuration below). PlatformIO:

```bash
git clone --recursive https://github.com/DRMx86/AirPlay-to-USB-Audio
cd AirPlay-to-USB-Audio
git checkout webradio-to-USB-Audio
pio run -e esp32s3 -t menuconfig     # Web Radio to USB DAC Audio -> Wi-Fi
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t monitor
```

ESP-IDF (>= 5.5):

```bash
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" menuconfig
idf.py -p <uart-port> flash monitor
```

The first build downloads `espressif/esp_audio_codec` (MP3 / AAC decoders) through the ESP-IDF
component manager, so it needs internet access.

This repository commits a generated `sdkconfig`, and it takes precedence over the
`sdkconfig.defaults*` files. If you built an earlier version in the same checkout and the
component manager complains, delete `managed_components/` (and `dependencies.lock`).

**Credentials and CI builds:** the Wi-Fi name and password are compiled into the firmware. The
GitHub Actions workflow builds from the committed `sdkconfig`, so a CI-built image has no Wi-Fi
credentials unless you commit them, which you should not do in a public repository. Build and
flash locally with your own settings instead.

## Configuration

`menuconfig` → **Web Radio to USB DAC Audio**:

| Option | Default | Notes |
| --- | --- | --- |
| Wi-Fi SSID / password | empty | Required. Leave the password empty for an open network. |
| Stream buffer size | 128 KB | Compressed audio buffered in PSRAM; about 8 s at 128 kbit/s. Rides out Wi-Fi dropouts. |
| Buffer before playback | 32 KB | Playback starts once this much is buffered. |
| Network timeout | 10 s | The player reconnects after three consecutive read timeouts. |
| Next-station button GPIO | -1 | Button to GND, internal pull-up. |
| USB DAC sample rate | 48 kHz | Rate sent to the dongle. Streams at other rates (usually 44.1 kHz) are resampled to it. |
| USB FIFO size | 60 ms | Buffer between the decoder and the USB transfers. |
| USB prefill | 20 ms | Audio buffered before the dongle is fed; sets the USB-side latency. |
| VBUS enable GPIO | -1 | GPIO that switches 5 V onto the host port, if your board has one. |

Keep the USB DAC sample rate at **48000**. The resampler is compiled out for a 44100 output, so
that setting only plays 44.1 kHz streams correctly.

## How it works

```
 net task ──▶ byte buffer (PSRAM) ──▶ decoder task ──▶ USB FIFO ──▶ isochronous OUT ──▶ DAC
```

- `main/net/wifi_sta.c` — Wi-Fi station with automatic reconnect; power save is off to avoid
  latency spikes.
- `main/radio/radio_player.c` — the **network task** opens the stream with `Icy-MetaData: 1`,
  follows redirects, strips the interleaved metadata (and logs the current track title) and
  fills the byte buffer. The **decoder task** feeds the buffer to the `esp_audio_codec` simple
  decoder (MP3 or AAC, picked from `Content-Type`), converts mono to stereo, resamples if the
  stream rate differs from the USB rate and writes to the USB output.
- Backpressure: the decoder only writes when the USB FIFO has room, so the byte buffer fills
  and TCP slows the server to playback speed. If the dongle is unplugged the audio is
  discarded at real-time speed so the stream stays live and resumes when it is plugged back in.
- A dropped connection is retried with a back-off (1 s up to 10 s). Audio keeps playing from the
  buffer while it reconnects; a station change flushes everything.
- `main/audio/audio_output_usb_host.c` — USB host backend using the ESP-IDF USB Host library
  directly. On connect it reads the descriptors, finds a stereo PCM isochronous-OUT
  alt-setting (16-bit preferred, 24/32-bit if that is all the dongle offers), sets the sample
  rate (UAC 2.0: on the clock sources; UAC 1.0: on the endpoint), selects the interface,
  unmutes the feature units and streams through isochronous transfers. Hot-plug is supported.
- `main/audio/audio_resample.c` — sinc resampler wrapper (`components/audio-resampler`).

The serial log prints a status line every 5 seconds: Wi-Fi state, station, codec and stream
format, stream buffer fill, reconnects, decode errors, whether a USB DAC is connected, FIFO fill
and overrun/underrun counts. When a dongle is plugged in, the log also lists its audio
alt-settings and the UAC version found, which is useful if a particular dongle does not work.

## Status

**Not yet built or run on hardware.** The sandbox this was written in could not reach the ESP-IDF
toolchain, so only the following was checked: the new code compiles cleanly with
`-Wall -Wextra` against the real `esp_audio_codec` headers (with stubs for the ESP-IDF APIs), and
the ICY metadata splitting and codec detection pass a host-side test at many chunk sizes. A real
`idf.py build`, a listening test with your dongle, and checking the stream's actual codec and
sample rate are still to do. Expect to fix small build issues on the first compile.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).
Provided as-is without warranty.
