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
- A Wi-Fi network (2.4 GHz) with internet access. It is set up from your phone, see below.
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

No Wi-Fi details go into the firmware: the radio asks for them on first boot (see
**Wi-Fi setup** below). PlatformIO:

```bash
git clone --recursive https://github.com/DRMx86/AirPlay-to-USB-Audio
cd AirPlay-to-USB-Audio
git checkout webradio-to-USB-Audio
pio run -e esp32s3 -t menuconfig     # optional: Web Radio to USB DAC Audio
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

The GitHub Actions workflow for this branch (`.github/workflows/webradio-usb-dac-build.yml`,
artifact `webradio-usb-dac-esp32s3-firmware`) builds from the committed `sdkconfig`. Because Wi-Fi
is set up on the device, a CI-built image works like a local one.

## Wi-Fi setup

On first boot (or whenever the saved network cannot be reached) the radio opens its own Wi-Fi
network called **WebRadio**:

1. On your phone or laptop, join the **WebRadio** network. The setup page opens by itself; if it
   does not, go to **http://192.168.4.1/**.
2. Pick your home network, enter its password and tap **Connect**.
3. The page confirms and shows the radio's new address. Switch your phone back to your normal
   Wi-Fi. The setup network closes a few seconds later and the radio starts playing.

The network is saved in the device's flash and used on every boot. If it cannot be joined after
`WIFI_MAX_RETRY` attempts at boot, the WebRadio network opens again (and the saved network is
retried every 5 minutes while nobody is using the page). Once the radio is on your network, the
same page is available at its IP address (shown in the serial log and on the page) to change or
**forget** the network. Use "Other network..." for a hidden network.

The setup page is dark and minimal, works offline (no internet is needed to load it) and has no
login. Anyone on the same network, or near the radio while its setup network is open, can use it,
so set `WIFI_AP_PASSWORD` in `menuconfig` if that matters to you.

## Volume

The volume **starts low at every boot** (default 10 %) and is applied *before* any audio is sent,
so a speaker can never blast at full volume when you plug it in. (An earlier version set every USB
volume control to 0 dB, which is the maximum on most devices, and that is what made the speaker
loud.) You can then raise it:

- **From the web page** at the radio's IP address: the slider changes the speaker's own USB
  volume control (UAC Feature Unit). The percentage covers the top 60 dB of the device's range,
  so the whole slider is usable. 0 % mutes.
- **If the speaker has no USB volume control**, the same slider applies a digital gain to the
  samples instead. The page says which one is in use.
- **With the speaker's own buttons**: those change the speaker's amplifier, independent of this
  setting. Whether they also move the USB volume depends on the speaker, and the radio does not
  read it back, so the slider may not follow them. When a speaker is plugged in, the serial log
  prints the volume range it reports (`FU 5 volume (master): -91.0 .. 0.0 dB, step 1.0 dB`), which
  shows how the speaker (such as a JBL Charge) exposes it.

The volume you set is kept until the next reboot, and re-applied if you unplug and replug the
speaker. Change the boot default with `USB_AUDIO_INITIAL_VOLUME_PERCENT` in `menuconfig`.

## Configuration

`menuconfig` → **Web Radio to USB DAC Audio**:

| Option | Default | Notes |
| --- | --- | --- |
| Setup access point name | WebRadio | Name of the setup network. |
| Setup access point password | empty | Empty = open setup network; 8-63 characters = WPA2. |
| Connection attempts before the portal | 5 | How often the saved network is tried at boot first. |
| Stream buffer size | 128 KB | Compressed audio buffered in PSRAM; about 8 s at 128 kbit/s. Rides out Wi-Fi dropouts. |
| Buffer before playback | 32 KB | Playback starts once this much is buffered. |
| Network timeout | 10 s | The player reconnects after three consecutive read timeouts. |
| Next-station button GPIO | -1 | Button to GND, internal pull-up. |
| USB DAC sample rate | 48 kHz | Rate sent to the dongle. Streams at other rates (usually 44.1 kHz) are resampled to it. |
| Initial volume | 10 % | Volume at every boot / plug-in. 0 starts muted. |
| USB FIFO size | 60 ms | Buffer between the decoder and the USB transfers. |
| USB prefill | 20 ms | Audio buffered before the dongle is fed; sets the USB-side latency. |
| VBUS enable GPIO | -1 | GPIO that switches 5 V onto the host port, if your board has one. |

Keep the USB DAC sample rate at **48000**. The resampler is compiled out for a 44100 output, so
that setting only plays 44.1 kHz streams correctly.

## How it works

```
 net task ──▶ byte buffer (PSRAM) ──▶ decoder task ──▶ USB FIFO ──▶ isochronous OUT ──▶ DAC
```

- `main/net/wifi_manager.c` — saved network first, setup access point + captive portal when
  that fails; credentials in NVS; automatic reconnect; power save is off to avoid latency spikes.
  `captive_dns.c` answers every DNS query with the radio's address so phones open the page by
  themselves; `portal.html` is the page (embedded in the firmware, no external files).
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
toolchain, so only the following was checked:

- the new code compiles cleanly with `-Wall -Wextra` against the real `esp_audio_codec` and USB
  host headers (with stubs for the rest of ESP-IDF);
- host-side tests pass for the ICY metadata splitter, the DNS responder, the form parser, JSON
  escaping, the volume mapping, reading a volume range over UAC 1.0 and UAC 2.0 control requests,
  and the digital-gain path;
- the setup page was rendered in a browser against a mocked device.

A real `idf.py build`, the Wi-Fi setup flow on a phone, and a listening test with your speaker are
still to do. Expect to fix small build issues on the first compile.

## Legal

**Non-commercial use only.** Commercial use requires explicit permission — see [LICENSE](LICENSE).
Provided as-is without warranty.
