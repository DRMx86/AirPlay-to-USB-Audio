/**
 * USB Audio Class HOST output -- ESP32-S3 drives a USB DAC dongle.
 *
 * Supports both UAC 1.0 and UAC 2.0 devices.  Espressif's usb_host_uac
 * component only handles UAC 1.0, so this backend talks to the native
 * ESP-IDF USB Host library directly (ported from the airplay-esp32 `main`
 * branch, where it is proven on UAC 2.0 devices such as the Apple USB-C
 * adapter and on UAC 1.0 DACs):
 *
 *   - registers a USB host client
 *   - on device connect: parses the active config descriptor and finds an
 *     audio STREAMING interface alt-setting that is stereo PCM on an
 *     isochronous OUT endpoint.  Only the standard AS interface / format /
 *     endpoint descriptors are read, so this works for UAC 1.0 and 2.0.
 *   - UAC 2.0: sets the sample rate on every Clock Source entity.
 *     UAC 1.0: sets the sample rate on the streaming endpoint.
 *   - claims the interface, sends SET_INTERFACE (enables the endpoint) and
 *     unmutes the Feature Units
 *   - streams PCM through a pool of isochronous OUT transfers, kept fed from
 *     a FIFO (silence on underrun).  16-bit PCM is expanded to 24/32-bit if
 *     the dongle only offers wider slots.
 *
 * Data path:
 *   capture task --audio_output_write()--> FIFO --iso OUT callbacks--> dongle
 *
 * The dongle's endpoint is assumed synchronous/adaptive (no feedback EP); the
 * nominal data rate is sent.  Fractional packet sizing makes 44.1 kHz exact.
 *
 * Wiring: the S3's native USB OTG port (GPIO19 D-, GPIO20 D+) with 5 V on
 * VBUS.  The USB-UART port cannot be used for this, and it takes over the
 * native USB-Serial/JTAG port, so flash and monitor over the UART port.
 * The ESP32-S3 USB is full-speed only (12 Mbps): high-speed-only UAC 2.0
 * devices will not work.
 */

#include "audio_output.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG         "audio_usbh"
#define OUTPUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ

#define BYTES_PER_MS ((size_t)(OUTPUT_RATE / 1000) * 4) /* 16-bit stereo */
#define FIFO_BYTES   (BYTES_PER_MS * CONFIG_USB_AUDIO_BUFFER_MS)
#define PRIME_BYTES  (BYTES_PER_MS * CONFIG_USB_AUDIO_PRIME_MS)

#if CONFIG_USB_AUDIO_PRIME_MS >= CONFIG_USB_AUDIO_BUFFER_MS
#error \
    "CONFIG_USB_AUDIO_PRIME_MS must be smaller than CONFIG_USB_AUDIO_BUFFER_MS"
#endif

#define USB_CORE     0
#define USB_LIB_PRIO 9
#define USB_CLI_PRIO 6

/* Isochronous OUT transfer pool. Full-speed: 1 packet per 1 ms frame. */
#define NUM_URBS        4
#define PACKETS_PER_URB 8 /* 8 ms per URB; 32 ms of transfers in flight */

/* Worst-case 16-bit PCM bytes popped per URB (one extra sample per packet). */
#define POP_MAX_BYTES (((OUTPUT_RATE / 1000) + 1) * PACKETS_PER_URB * 4)

/* ── USB / streaming state ───────────────────────────────────────────────── */
static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t s_dev = NULL;
static volatile bool s_iso_running = false; /* iso OUT transfers in flight */
static volatile bool s_primed = false;      /* FIFO filled, audio being sent */

static uint8_t s_out_iface = 0;
static uint8_t s_out_alt = 0;
static uint8_t s_out_ep = 0;
static uint16_t s_out_mps = 0;
/* Fractional iso packet sizing so OUTPUT_RATE can be 44.1 kHz exactly:
 * packets carry 44 samples most 1 ms frames and 45 every ~10th. */
static int s_frame_bytes = 4;  /* bytes per audio frame on the wire */
static int s_pkt_base = 0;     /* floor samples/frame = OUTPUT_RATE / 1000 */
static int s_pkt_frac = 0;     /* OUTPUT_RATE % 1000 */
static int s_frac_accum = 0;   /* fractional-sample accumulator */
static uint8_t s_ac_iface = 0; /* AudioControl interface (class requests) */
static uint8_t s_clock_id = 0; /* UAC2 clock of the USB-streaming terminal */
static uint8_t s_clock_ids[4]; /* all UAC2 Clock Source entity IDs */
static int s_clock_count = 0;
static uint8_t s_fu_ids[8]; /* Feature Unit IDs found in the AC interface */
static int s_fu_count = 0;
static uint8_t s_mixer_id = 0; /* Mixer Unit ID (0 = none) */
static int s_out_subslot = 2;  /* device sample size in bytes: 2=16-bit, 3, 4 */
static uint8_t s_uac_ver = 0;  /* UAC spec major version: 1 or 2 */

static usb_transfer_t *s_urb[NUM_URBS];
static SemaphoreHandle_t s_ctrl_sem = NULL; /* control transfer completion */
static SemaphoreHandle_t s_ctrl_lock =
    NULL; /* one control transfer at a time */
static int16_t s_pop_buf[POP_MAX_BYTES / 2]; /* only touched in iso callbacks */

/* connect/disconnect handshake from the client callback to the setup task */
static volatile bool s_connect_pending = false;
static volatile bool s_disconnect_pending = false;
static volatile uint8_t s_pending_addr = 0;

/* 16-bit stereo PCM FIFO.  Producer: capture task (audio_output_write).
 * Consumer: iso OUT completion callbacks, which must never block. */
static StreamBufferHandle_t s_pcm = NULL;

/* telemetry */
static volatile uint32_t s_overruns = 0;
static volatile uint32_t s_underruns = 0;
static volatile uint32_t s_xfer_done = 0, s_xfer_err = 0, s_pkt_err = 0;

/* ── Optional VBUS enable ────────────────────────────────────────────────── */
static void usb_host_vbus_enable(void) {
#if CONFIG_USB_HOST_VBUS_EN_GPIO >= 0
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_USB_HOST_VBUS_EN_GPIO,
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io);
  gpio_set_level(CONFIG_USB_HOST_VBUS_EN_GPIO, 1);
  ESP_LOGI(TAG, "VBUS enable GPIO%d -> HIGH", CONFIG_USB_HOST_VBUS_EN_GPIO);
#endif
}

/* ── USB host library daemon ─────────────────────────────────────────────── */
static void usb_lib_task(void *arg) {
  (void)arg;
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

/* ── Descriptor parsing ──────────────────────────────────────────────────── */
/* Walk the active config descriptor and find an audio STREAMING interface
 * alt-setting that is stereo PCM on an isochronous OUT endpoint.  Also records
 * the AudioControl topology (clocks, feature units, mixer) used to set the
 * sample rate and unmute.  Returns true if a usable alt-setting was found. */
static bool find_speaker_altsetting(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg;
  const uint8_t *end = p + cfg->wTotalLength;
  p += p[0]; /* skip the 9-byte config descriptor */

  int iface = -1, alt = -1;
  bool cur_is_as = false; /* current alt is an AudioStreaming interface */
  bool cur_is_ac = false; /* current interface is AudioControl */
  int as_channels = 0, as_bits = 0, as_subslot = 0;
  bool found_speaker = false;
  s_ac_iface = 0;
  s_uac_ver = 0;
  s_out_subslot = 2;
  s_clock_id = 0;
  s_clock_count = 0;
  s_fu_count = 0;
  s_mixer_id = 0;

  while (p + 2 <= end) {
    uint8_t len = p[0], type = p[1];
    if (len < 2 || p + len > end) {
      break;
    }

    if (type == 0x04) { /* INTERFACE */
      iface = p[2];
      alt = p[3];
      uint8_t cls = p[5], sub = p[6], proto = p[7];
      cur_is_ac = (cls == 0x01 && sub == 0x01); /* AUDIO / AUDIOCONTROL */
      cur_is_as = (cls == 0x01 && sub == 0x02); /* AUDIO / AUDIOSTREAMING */
      if (cur_is_ac) {
        s_ac_iface = (uint8_t)iface;
        s_uac_ver = (proto == 0x20) ? 2 : 1; /* protocol 0x20 = UAC 2.0 */
      }
      as_channels = 0;
      as_bits = 0;
      as_subslot = 0;
    } else if (cur_is_ac && type == 0x24) { /* CS_INTERFACE (AudioControl) */
      if (p[2] == 0x02 && len >= 8) {
        /* INPUT_TERMINAL (UAC2): clock of the USB-streaming terminal.
         * wTerminalType @4, bCSourceID @7 */
        uint16_t ttype = (uint16_t)(p[4] | (p[5] << 8));
        if (ttype == 0x0101) { /* USB Streaming */
          s_clock_id = p[7];
        }
      } else if (p[2] == 0x04 && len >= 5) {
        s_mixer_id = p[3]; /* MIXER_UNIT: bUnitID @3 */
      } else if (p[2] == 0x06 && len >= 4 && s_fu_count < 8) {
        s_fu_ids[s_fu_count++] = p[3]; /* FEATURE_UNIT: bUnitID @3 */
      } else if (p[2] == 0x0a && len >= 4 &&
                 s_clock_count < (int)sizeof(s_clock_ids)) {
        /* CLOCK_SOURCE (UAC2): bClockID @3 - set every clock's rate */
        s_clock_ids[s_clock_count++] = p[3];
      }
    } else if (cur_is_as && type == 0x24) { /* CS_INTERFACE (AudioStreaming) */
      uint8_t subtype = p[2];
      if (subtype == 0x01) {
        /* AS_GENERAL: UAC2 has bNrChannels @10 (UAC1 has it in FORMAT_TYPE) */
        if (s_uac_ver == 2 && len >= 11) {
          as_channels = p[10];
        }
      } else if (subtype == 0x02) {
        /* FORMAT_TYPE_I: UAC2 -> bSubslotSize@4, bBitResolution@5;
         *                UAC1 -> bNrChannels@4, bSubframeSize@5, bBitRes@6 */
        if (s_uac_ver == 2) {
          if (len >= 6) {
            as_subslot = p[4];
            as_bits = p[5];
          }
        } else if (len >= 7) {
          as_channels = p[4];
          as_subslot = p[5];
          as_bits = p[6];
        }
      }
    } else if (cur_is_as && type == 0x05) { /* ENDPOINT */
      uint8_t ep_addr = p[2], ep_attr = p[3];
      uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
      bool is_out = !(ep_addr & 0x80);
      bool is_iso = (ep_attr & 0x03) == 0x01;
      ESP_LOGI(TAG,
               "  AS iface=%d alt=%d: ch=%d bits=%d sub=%d ep=0x%02x iso=%d "
               "out=%d sync=%d use=%d mps=%u",
               iface, alt, as_channels, as_bits, as_subslot, ep_addr, is_iso,
               is_out, (ep_attr >> 2) & 0x03, (ep_attr >> 4) & 0x03, mps);
      if (is_iso && is_out && mps > 0 && as_channels == 2 && as_subslot >= 2 &&
          as_subslot <= 4) {
        /* Stereo PCM iso OUT = the speaker path.  Prefer 16-bit (subslot 2,
         * no conversion); otherwise accept 24-/32-bit. */
        bool better = !found_speaker || (s_out_subslot != 2 && as_subslot == 2);
        if (better) {
          s_out_iface = (uint8_t)iface;
          s_out_alt = (uint8_t)alt;
          s_out_ep = ep_addr;
          s_out_mps = mps;
          s_out_subslot = as_subslot;
          found_speaker = true;
        }
      }
    }
    p += len;
  }
  return found_speaker;
}

/* ── Isochronous OUT streaming ───────────────────────────────────────────── */
/* Size each iso packet for the (possibly fractional) rate; sets per-packet
 * num_bytes and xfer->num_bytes.  Returns the number of audio frames. */
static int fill_out_packets(usb_transfer_t *xfer) {
  int total_bytes = 0, frames = 0;
  for (int j = 0; j < PACKETS_PER_URB; j++) {
    int samples = s_pkt_base;
    s_frac_accum += s_pkt_frac;
    if (s_frac_accum >= 1000) {
      samples++;
      s_frac_accum -= 1000;
    }
    int bytes = samples * s_frame_bytes;
    xfer->isoc_packet_desc[j].num_bytes = bytes;
    total_bytes += bytes;
    frames += samples;
  }
  xfer->num_bytes = total_bytes;
  return frames;
}

/* Expand n16 16-bit PCM samples to the device's subslot size, left-justified
 * (e.g. 16-bit -> 24-bit = value << 8). Writes n16 * subslot bytes. */
static void expand_pcm(const int16_t *src, int n16, uint8_t *dst, int subslot) {
  int shift = 8 * (subslot - 2);
  uint8_t *o = dst;
  for (int i = 0; i < n16; i++) {
    int32_t v = (int32_t)((uint32_t)(int32_t)src[i] << shift);
    for (int b = 0; b < subslot; b++) {
      *o++ = (uint8_t)(v >> (8 * b));
    }
  }
}

/* Fill the URB payload with `frames` frames from the FIFO (or silence).
 * Runs in the USB client task; must not block. */
static void fill_out_payload(usb_transfer_t *xfer, int frames) {
  size_t want = (size_t)frames * 4; /* 16-bit stereo in the FIFO */
  uint8_t *dst =
      (s_out_subslot == 2) ? xfer->data_buffer : (uint8_t *)s_pop_buf;
  size_t got = 0;

  if (!s_primed && xStreamBufferBytesAvailable(s_pcm) >= PRIME_BYTES) {
    s_primed = true;
  }
  if (s_primed) {
    got = xStreamBufferReceive(s_pcm, dst, want, 0);
    if (got < want) {
      s_primed = false; /* ran dry: go back to priming */
      s_underruns++;
    }
  }
  if (got < want) {
    memset(dst + got, 0, want - got); /* silence */
  }
  if (s_out_subslot != 2) {
    expand_pcm(s_pop_buf, frames * 2, xfer->data_buffer, s_out_subslot);
  }
}

static void out_xfer_cb(usb_transfer_t *xfer) {
  if (!s_iso_running) {
    return; /* tearing down - do not resubmit */
  }
  s_xfer_done++;
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    s_xfer_err++;
  }
  for (int j = 0; j < PACKETS_PER_URB; j++) {
    if (xfer->isoc_packet_desc[j].status != USB_TRANSFER_STATUS_COMPLETED) {
      s_pkt_err++;
    }
  }

  int frames = fill_out_packets(xfer);
  fill_out_payload(xfer, frames);

  if ((s_xfer_done % 1250) == 0) { /* ~ every 10 s (1250 urbs * 8 ms) */
    ESP_LOGI(TAG, "tlm xfers=%lu xerr=%lu pkterr=%lu",
             (unsigned long)s_xfer_done, (unsigned long)s_xfer_err,
             (unsigned long)s_pkt_err);
  }

  /* Resubmit regardless of transient status so the iso stream keeps flowing;
   * a real disconnect arrives via the DEV_GONE client event. */
  esp_err_t e = usb_host_transfer_submit(xfer);
  if (e != ESP_OK && (s_xfer_done % 1250) == 0) {
    ESP_LOGW(TAG, "resubmit iso OUT: %s", esp_err_to_name(e));
  }
}

static esp_err_t start_streaming(void) {
  /* Fractional packets: e.g. 44.1 kHz = 44 samples/frame, 45 every ~10th.
   * Allocate the worst case (one extra sample/packet), capped at the MPS. */
  s_frame_bytes = 2 * s_out_subslot; /* 4 = 16-bit, 6 = 24-bit (stereo) */
  s_pkt_base = OUTPUT_RATE / 1000;
  s_pkt_frac = OUTPUT_RATE % 1000;
  s_frac_accum = 0;
  int alloc = (s_pkt_base + 1) * s_frame_bytes * PACKETS_PER_URB;
  if (alloc > (int)s_out_mps * PACKETS_PER_URB) {
    alloc = (int)s_out_mps * PACKETS_PER_URB;
  }

  xStreamBufferReset(s_pcm);
  s_primed = false;
  s_iso_running = true;
  for (int i = 0; i < NUM_URBS; i++) {
    ESP_RETURN_ON_ERROR(
        usb_host_transfer_alloc(alloc, PACKETS_PER_URB, &s_urb[i]), TAG,
        "transfer_alloc");
    s_urb[i]->device_handle = s_dev;
    s_urb[i]->bEndpointAddress = s_out_ep;
    s_urb[i]->callback = out_xfer_cb;
    s_urb[i]->context = NULL;
    memset(s_urb[i]->data_buffer, 0, alloc); /* prime with silence */
    fill_out_packets(s_urb[i]);              /* sets packet sizes + num_bytes */
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_urb[i]), TAG, "submit");
  }
  ESP_LOGI(TAG,
           "Streaming: ep=0x%02x rate=%d (%d+frac samp/frame, %d-bit slots) "
           "x %d pkt x %d urbs",
           s_out_ep, OUTPUT_RATE, s_pkt_base, s_out_subslot * 8,
           PACKETS_PER_URB, NUM_URBS);
  return ESP_OK;
}

/* ── Control transfers ───────────────────────────────────────────────────── */
static void ctrl_done_cb(usb_transfer_t *t) {
  (void)t;
  xSemaphoreGive(s_ctrl_sem);
}

/* Blocking control transfer on EP0. MUST be called from a task other than the
 * USB client task (that task dispatches this transfer's completion callback).
 */
static esp_err_t ctrl_xfer_locked(uint8_t reqtype, uint8_t req, uint16_t val,
                                  uint16_t idx, uint8_t *data, uint16_t len) {
  usb_transfer_t *x = NULL;
  uint16_t buflen = len ? len : 1;
  if (usb_host_transfer_alloc(8 + buflen, 0, &x) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }
  usb_setup_packet_t *s = (usb_setup_packet_t *)x->data_buffer;
  s->bmRequestType = reqtype;
  s->bRequest = req;
  s->wValue = val;
  s->wIndex = idx;
  s->wLength = len;
  if (data && len) {
    memcpy(x->data_buffer + 8, data, len);
  }
  x->num_bytes = 8 + len;
  x->device_handle = s_dev;
  x->bEndpointAddress = 0;
  x->callback = ctrl_done_cb;
  x->context = NULL;
  esp_err_t e = usb_host_transfer_submit_control(s_client, x);
  if (e == ESP_OK) {
    if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
      e = ESP_ERR_TIMEOUT;
    } else {
      e = (x->status == USB_TRANSFER_STATUS_COMPLETED) ? ESP_OK : ESP_FAIL;
      if (e == ESP_OK && (reqtype & 0x80) && data && len &&
          x->actual_num_bytes > 8) {
        /* Device-to-host: the payload follows the 8-byte setup packet. */
        size_t got = (size_t)x->actual_num_bytes - 8;
        memcpy(data, x->data_buffer + 8, got < len ? got : len);
      }
    }
  }
  usb_host_transfer_free(x);
  return e;
}

/* Control transfers are serialised: the setup task and the volume control
 * (called from any task) share one completion semaphore. */
static esp_err_t ctrl_xfer_sync(uint8_t reqtype, uint8_t req, uint16_t val,
                                uint16_t idx, uint8_t *data, uint16_t len) {
  if (!s_ctrl_lock ||
      xSemaphoreTake(s_ctrl_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t e = s_dev ? ctrl_xfer_locked(reqtype, req, val, idx, data, len)
                      : ESP_ERR_INVALID_STATE;
  xSemaphoreGive(s_ctrl_lock);
  return e;
}

/* UAC2: SET_CUR CS_SAM_FREQ_CONTROL on a Clock Source entity (4-byte rate). */
static esp_err_t uac2_set_clock_hz(uint8_t clk_id, uint32_t hz) {
  uint8_t d[4] = {(uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16),
                  (uint8_t)(hz >> 24)};
  return ctrl_xfer_sync(0x21, 0x01, 0x0100,
                        (uint16_t)((clk_id << 8) | s_ac_iface), d, 4);
}

/* UAC1: SAMPLING_FREQ_CONTROL on the streaming ENDPOINT (3-byte rate).  The
 * endpoint exists only after the streaming alt is selected, so call this after
 * SET_INTERFACE. */
static esp_err_t uac1_set_ep_rate(uint8_t ep, uint32_t hz) {
  uint8_t d[3] = {(uint8_t)hz, (uint8_t)(hz >> 8), (uint8_t)(hz >> 16)};
  return ctrl_xfer_sync(0x22, 0x01, 0x0100, ep, d, 3);
}

static esp_err_t uac_fu_set_mute(uint8_t fu_id, bool mute) {
  uint8_t d = mute ? 1 : 0; /* MUTE_CONTROL (0x01), channel 0 (master) */
  return ctrl_xfer_sync(0x21, 0x01, 0x0100,
                        (uint16_t)((fu_id << 8) | s_ac_iface), &d, 1);
}

/* ── Volume ────────────────────────────────────────────────────────────────
 * The volume starts LOW (CONFIG_USB_AUDIO_INITIAL_VOLUME_PERCENT) every boot
 * and is applied before the stream starts, so a speaker can never blast at
 * full volume.  A 0-100 % value is mapped onto the top VOLUME_RANGE_DB of the
 * device's own Feature Unit range (UAC ranges often reach -127 dB, which would
 * leave most of a slider silent).  Devices without a Feature Unit volume get
 * the same curve as a digital gain on the PCM instead. */
#define VOLUME_RANGE_DB 60
/* Digital-gain range.  Smaller than the device range so that the low end of the
 * slider is still audible and not buried in 16-bit quantisation noise. */
#define SW_VOLUME_RANGE_DB 40

typedef struct {
  uint8_t id;
  uint8_t mode; /* 0 = no volume control, 1 = master, 2 = per channel */
  int16_t min, max, res; /* 1/256 dB units, as in the UAC descriptors */
} fu_vol_t;

static fu_vol_t s_fu_vol[sizeof(s_fu_ids)];
static volatile int s_volume_pct = CONFIG_USB_AUDIO_INITIAL_VOLUME_PERCENT;
static volatile bool s_hw_volume;      /* some Feature Unit has a volume */
static volatile int32_t s_sw_gain_q15; /* 32768 = unity */

static esp_err_t uac_fu_set_volume(uint8_t fu_id, uint8_t channel,
                                   int16_t db_q8) {
  uint8_t d[2] = {(uint8_t)db_q8, (uint8_t)(db_q8 >> 8)};
  /* VOLUME_CONTROL (0x02); channel 0 = master */
  return ctrl_xfer_sync(0x21, 0x01, (uint16_t)(0x0200 | channel),
                        (uint16_t)((fu_id << 8) | s_ac_iface), d, 2);
}

static int16_t le16(const uint8_t *p) {
  return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

/* Volume range of one channel of a Feature Unit.  UAC2 has a single RANGE
 * request; UAC1 has separate GET_MIN / GET_MAX / GET_RES. */
static esp_err_t uac_fu_get_range(uint8_t fu_id, uint8_t channel, int16_t *mn,
                                  int16_t *mx, int16_t *rs) {
  uint16_t val = (uint16_t)(0x0200 | channel);
  uint16_t idx = (uint16_t)((fu_id << 8) | s_ac_iface);
  if (s_uac_ver == 2) {
    uint8_t d[8] = {0}; /* wNumSubRanges, then MIN, MAX, RES of the first */
    esp_err_t e = ctrl_xfer_sync(0xA1, 0x02, val, idx, d, sizeof(d));
    if (e != ESP_OK) {
      return e;
    }
    if (d[0] == 0 && d[1] == 0) {
      return ESP_ERR_NOT_FOUND;
    }
    *mn = le16(d + 2);
    *mx = le16(d + 4);
    *rs = le16(d + 6);
  } else {
    static const uint8_t req[3] = {0x82, 0x83, 0x84}; /* GET_MIN/MAX/RES */
    int16_t v[3];
    for (int i = 0; i < 3; i++) {
      uint8_t b[2] = {0, 0};
      esp_err_t e = ctrl_xfer_sync(0xA1, req[i], val, idx, b, sizeof(b));
      if (e != ESP_OK) {
        return e;
      }
      v[i] = le16(b);
    }
    *mn = v[0];
    *mx = v[1];
    *rs = v[2];
  }
  if (*mx <= *mn) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (*rs <= 0) {
    *rs = 1;
  }
  return ESP_OK;
}

/* 0..100 % -> Feature Unit value, snapped to the device's step size. */
static int16_t volume_value(const fu_vol_t *f, int pct) {
  int range = f->max - f->min;
  int window = VOLUME_RANGE_DB * 256;
  int lo = f->max - (range < window ? range : window);
  int v = lo + (int)(((int64_t)(f->max - lo) * pct) / 100);
  v = f->min + ((v - f->min + f->res / 2) / f->res) * f->res;
  if (v > f->max) {
    v = f->max;
  }
  if (v < f->min) {
    v = f->min;
  }
  return (int16_t)v;
}

/* Push the current volume to every Feature Unit.  0 % also mutes. */
static void volume_apply_hw(void) {
  int pct = s_volume_pct;
  for (int i = 0; i < s_fu_count; i++) { /* volume first, then (un)mute */
    const fu_vol_t *f = &s_fu_vol[i];
    if (f->mode == 1) {
      uac_fu_set_volume(f->id, 0, volume_value(f, pct));
    } else if (f->mode == 2) {
      uac_fu_set_volume(f->id, 1, volume_value(f, pct));
      uac_fu_set_volume(f->id, 2, volume_value(f, pct));
    }
  }
  for (int i = 0; i < s_fu_count; i++) {
    uac_fu_set_mute(s_fu_ids[i], pct == 0);
  }
}

/* 0 dB (unity) snapped to the device's step size and range. */
static int16_t unity_value(const fu_vol_t *f) {
  int v = f->min + ((0 - f->min + f->res / 2) / f->res) * f->res;
  if (v > f->max) {
    v = f->max;
  }
  if (v < f->min) {
    v = f->min;
  }
  return (int16_t)v;
}

/* When the web page controls the volume digitally, leave the DAC's own volume
 * at a fixed, known level (0 dB) and unmuted, so it can neither swallow the
 * slider nor add its own attenuation/gain on top of it. */
static void volume_pin_hw_unity(void) {
  for (int i = 0; i < s_fu_count; i++) {
    const fu_vol_t *f = &s_fu_vol[i];
    if (f->mode == 1) {
      uac_fu_set_volume(f->id, 0, unity_value(f));
    } else if (f->mode == 2) {
      uac_fu_set_volume(f->id, 1, unity_value(f));
      uac_fu_set_volume(f->id, 2, unity_value(f));
    }
  }
  for (int i = 0; i < s_fu_count; i++) {
    uac_fu_set_mute(s_fu_ids[i], false);
  }
}

/* Digital gain used when the device has no volume control of its own. */
static void volume_update_sw_gain(void) {
  int pct = s_volume_pct;
  if (s_hw_volume || pct >= 100) {
    s_sw_gain_q15 = 32768;
  } else if (pct <= 0) {
    s_sw_gain_q15 = 0;
  } else {
    float db = -(float)SW_VOLUME_RANGE_DB * (float)(100 - pct) / 100.0f;
    s_sw_gain_q15 = (int32_t)(32768.0f * powf(10.0f, db / 20.0f));
  }
}

/* "-12.5" style text for logs (avoids float printf). */
static void fmt_db(char *buf, size_t n, int16_t q8) {
  int t = q8 * 10 / 256; /* tenths of a dB */
  snprintf(buf, n, "%s%d.%d", t < 0 ? "-" : "", abs(t) / 10, abs(t) % 10);
}

static esp_err_t uac_mixer_set_db(uint8_t mixer_id, uint8_t in_ch,
                                  uint8_t out_ch, int16_t db_q8) {
  uint8_t d[2] = {(uint8_t)db_q8, (uint8_t)(db_q8 >> 8)};
  /* wValue = (input channel number << 8) | output channel number */
  return ctrl_xfer_sync(0x21, 0x01, (uint16_t)((in_ch << 8) | out_ch),
                        (uint16_t)((mixer_id << 8) | s_ac_iface), d, 2);
}

/* ── Device setup / teardown ─────────────────────────────────────────────── */
static void setup_device(uint8_t addr) {
  if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
    ESP_LOGE(TAG, "device_open(addr=%u) failed", addr);
    s_dev = NULL;
    return;
  }
  const usb_config_desc_t *cfg = NULL;
  if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK || !cfg) {
    ESP_LOGE(TAG, "get_active_config_descriptor failed");
    goto fail_close;
  }
  if (!find_speaker_altsetting(cfg)) {
    ESP_LOGE(TAG, "No stereo PCM iso OUT alt-setting found (not a USB audio "
                  "output device, or it needs high-speed USB)");
    goto fail_close;
  }
  ESP_LOGI(TAG,
           "UAC %u.0 DAC: iface=%u alt=%u ep=0x%02x mps=%u slot=%d-bit | AC "
           "iface=%u clocks=%d FUs=%d mixer=%u",
           s_uac_ver, s_out_iface, s_out_alt, s_out_ep, s_out_mps,
           s_out_subslot * 8, s_ac_iface, s_clock_count, s_fu_count,
           s_mixer_id);

  /* 1) UAC2: set every clock source's sample rate BEFORE selecting a streaming
   * alt.  UAC1 has no clock entities - its rate is set on the endpoint after
   * SET_INTERFACE (below). */
  if (s_uac_ver == 2) {
    for (int i = 0; i < s_clock_count; i++) {
      esp_err_t e = uac2_set_clock_hz(s_clock_ids[i], OUTPUT_RATE);
      ESP_LOGI(TAG, "set clock id=%u %d Hz: %s", s_clock_ids[i], OUTPUT_RATE,
               esp_err_to_name(e));
    }
    if (s_clock_count == 0 && s_clock_id) {
      uac2_set_clock_hz(s_clock_id, OUTPUT_RATE);
    }
  }

  /* 2) Claim the streaming interface+alt.  NOTE: usb_host_interface_claim()
   * only configures the HOST-side pipes - it does NOT send SET_INTERFACE to the
   * device.  Without the explicit SET_INTERFACE below the device's streaming
   * endpoint stays on alt 0 (disabled) and every iso-OUT packet is silently
   * dropped. */
  if (usb_host_interface_claim(s_client, s_dev, s_out_iface, s_out_alt) !=
      ESP_OK) {
    ESP_LOGE(TAG, "interface_claim(iface=%u alt=%u) failed", s_out_iface,
             s_out_alt);
    goto fail_close;
  }
  esp_err_t si = ctrl_xfer_sync(0x01, 0x0b, s_out_alt, s_out_iface, NULL, 0);
  ESP_LOGI(TAG, "SET_INTERFACE(iface=%u alt=%u): %s", s_out_iface, s_out_alt,
           esp_err_to_name(si));
  /* UAC1: now that the endpoint exists, set its sampling frequency. */
  if (s_uac_ver == 1) {
    esp_err_t er = uac1_set_ep_rate(s_out_ep, OUTPUT_RATE);
    ESP_LOGI(TAG, "UAC1 set ep 0x%02x rate %d Hz: %s", s_out_ep, OUTPUT_RATE,
             esp_err_to_name(er));
  }

  /* 3) Volume.  Find which Feature Units have a volume control, set the LOW
   * initial volume, then unmute.  (This used to set 0 dB, which is the maximum
   * on most devices.)  Missing mute/volume controls are normal and only
   * logged. */
  s_hw_volume = false;
  bool fu_found = false;
  for (int i = 0; i < s_fu_count; i++) {
    fu_vol_t *f = &s_fu_vol[i];
    memset(f, 0, sizeof(*f));
    f->id = s_fu_ids[i];
    int16_t mn = 0;
    int16_t mx = 0;
    int16_t rs = 0;
    if (uac_fu_get_range(f->id, 0, &mn, &mx, &rs) == ESP_OK) {
      f->mode = 1;
    } else if (uac_fu_get_range(f->id, 1, &mn, &mx, &rs) == ESP_OK) {
      f->mode = 2;
    }
    if (f->mode) {
      f->min = mn;
      f->max = mx;
      f->res = rs;
      fu_found = true;
      char a[12];
      char b[12];
      char c[12];
      fmt_db(a, sizeof(a), mn);
      fmt_db(b, sizeof(b), mx);
      fmt_db(c, sizeof(c), rs);
      ESP_LOGI(TAG, "FU %u volume (%s): %s .. %s dB, step %s dB", f->id,
               f->mode == 1 ? "master" : "per channel", a, b, c);
    } else {
      ESP_LOGI(TAG, "FU %u has no volume control", f->id);
    }
  }
#ifdef CONFIG_USB_AUDIO_DEVICE_VOLUME
  s_hw_volume = fu_found; /* use the DAC's own volume control */
#else
  s_hw_volume = false; /* always use digital gain; pin the DAC at 0 dB */
#endif
  volume_update_sw_gain();
  if (s_hw_volume) {
    volume_apply_hw();
  } else if (fu_found) {
    volume_pin_hw_unity();
  }
  ESP_LOGI(TAG, "Volume %d%% (%s)", (int)s_volume_pct,
           s_hw_volume ? "device volume" : "digital gain");

  /* 3b) Route USB stereo through a mixer unit, if there is one. */
  if (s_mixer_id) {
    esp_err_t a = uac_mixer_set_db(s_mixer_id, 1, 1, 0); /* USB L -> out L */
    esp_err_t b = uac_mixer_set_db(s_mixer_id, 2, 2, 0); /* USB R -> out R */
    ESP_LOGI(TAG, "mixer %u (1->1):%s (2->2):%s", s_mixer_id,
             esp_err_to_name(a), esp_err_to_name(b));
  }

  /* 4) Start the isochronous OUT stream. */
  if (start_streaming() != ESP_OK) {
    ESP_LOGE(TAG, "start_streaming failed");
    s_iso_running = false;
    for (int i = 0; i < NUM_URBS; i++) {
      if (s_urb[i]) {
        usb_host_transfer_free(s_urb[i]);
        s_urb[i] = NULL;
      }
    }
    usb_host_interface_release(s_client, s_dev, s_out_iface);
    goto fail_close;
  }
  ESP_LOGI(TAG, "USB DAC streaming started");
  return;

fail_close:
  s_hw_volume = false;
  volume_update_sw_gain();
  usb_host_device_close(s_client, s_dev);
  s_dev = NULL;
}

static void teardown_device(void) {
  s_iso_running = false;
  s_primed = false;
  s_hw_volume = false;
  volume_update_sw_gain();
  vTaskDelay(pdMS_TO_TICKS(5)); /* let in-flight callbacks settle */
  for (int i = 0; i < NUM_URBS; i++) {
    if (s_urb[i]) {
      usb_host_transfer_free(s_urb[i]);
      s_urb[i] = NULL;
    }
  }
  if (s_dev) {
    usb_host_interface_release(s_client, s_dev, s_out_iface);
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
  }
  ESP_LOGI(TAG, "USB DAC removed, waiting for a new one");
}

/* ── USB client (NEW_DEV / DEV_GONE) ─────────────────────────────────────── */
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
  (void)arg;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!s_dev && !s_connect_pending) {
      s_pending_addr = msg->new_dev.address;
      s_connect_pending = true;
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    s_disconnect_pending = true;
  }
}

static void usb_client_task(void *arg) {
  (void)arg;
  const usb_host_client_config_t cfg = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
  };
  if (usb_host_client_register(&cfg, &s_client) != ESP_OK) {
    ESP_LOGE(TAG, "client_register failed");
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "USB host ready; waiting for a USB DAC dongle");
  /* Pump events continuously: dispatches NEW_DEV / DEV_GONE, the setup task's
   * control-transfer completions, and the iso OUT completions. */
  while (true) {
    usb_host_client_handle_events(s_client, portMAX_DELAY);
  }
}

/* Device setup/teardown runs on its own task (not the client task) so the
 * blocking control transfers in setup_device() can wait while the client task
 * keeps dispatching their completion callbacks. */
static void usb_setup_task(void *arg) {
  (void)arg;
  while (true) {
    if (s_disconnect_pending) {
      s_disconnect_pending = false;
      teardown_device();
    }
    if (s_connect_pending && s_client) {
      s_connect_pending = false;
      setup_device(s_pending_addr);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/* ── Public API (audio_output.h) ─────────────────────────────────────────── */
esp_err_t audio_output_init(void) {
  ESP_LOGI(TAG, "USB host UAC output (%d Hz, buffer %d ms, prime %d ms)",
           OUTPUT_RATE, CONFIG_USB_AUDIO_BUFFER_MS, CONFIG_USB_AUDIO_PRIME_MS);

  s_pcm = xStreamBufferCreate(FIFO_BYTES, 1);
  s_ctrl_sem = xSemaphoreCreateBinary();
  s_ctrl_lock = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_pcm && s_ctrl_sem && s_ctrl_lock, ESP_ERR_NO_MEM, TAG,
                      "no memory");
  volume_update_sw_gain(); /* start quiet even before a DAC is attached */

  usb_host_vbus_enable();
  const usb_host_config_t host_cfg = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

  xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, USB_LIB_PRIO,
                          NULL, USB_CORE);
  xTaskCreatePinnedToCore(usb_client_task, "usb_cli", 5120, NULL, USB_CLI_PRIO,
                          NULL, USB_CORE);
  xTaskCreatePinnedToCore(usb_setup_task, "usb_setup", 5120, NULL, USB_CLI_PRIO,
                          NULL, USB_CORE);
  return ESP_OK;
}

size_t audio_output_write(const int16_t *pcm, size_t frames) {
  StreamBufferHandle_t sb = s_pcm;
  if (!sb || !pcm || frames == 0) {
    return 0;
  }
  size_t bytes = frames * 4;
  /* Only whole frames: the consumer must never see half a stereo frame. */
  size_t space = xStreamBufferSpacesAvailable(sb) & ~(size_t)3;
  size_t n = bytes < space ? bytes : space;
  if (n < bytes) {
    s_overruns++;
  }
  if (n > 0) {
    int32_t gain = s_sw_gain_q15;
    if (gain >= 32768) {
      n = xStreamBufferSend(sb, pcm, n, 0);
    } else { /* no device volume control: scale the samples ourselves */
      int16_t tmp[128];
      size_t done = 0;
      while (done < n) {
        size_t chunk = n - done;
        if (chunk > sizeof(tmp)) {
          chunk = sizeof(tmp);
        }
        const int16_t *src = (const int16_t *)((const uint8_t *)pcm + done);
        for (size_t i = 0; i < chunk / 2; i++) {
          tmp[i] = (int16_t)(((int32_t)src[i] * gain) >> 15);
        }
        size_t sent = xStreamBufferSend(sb, tmp, chunk, 0);
        done += sent;
        if (sent < chunk) {
          break;
        }
      }
      n = done;
    }
  }
  return n / 4;
}

void audio_output_set_volume_percent(int percent) {
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  s_volume_pct = percent;
  volume_update_sw_gain();
  if (s_hw_volume && s_iso_running) {
    volume_apply_hw();
  }
}

int audio_output_get_volume_percent(void) {
  return s_volume_pct;
}

void audio_output_flush(void) {
  s_primed = false;
  if (s_pcm) {
    xStreamBufferReset(s_pcm);
  }
}

void audio_output_get_stats(audio_output_stats_t *stats) {
  if (!stats) {
    return;
  }
  StreamBufferHandle_t sb = s_pcm;
  stats->connected = s_iso_running;
  stats->streaming = s_iso_running && s_primed;
  stats->capacity_bytes = FIFO_BYTES;
  stats->fill_bytes = sb ? (uint32_t)xStreamBufferBytesAvailable(sb) : 0;
  stats->overruns = s_overruns;
  stats->underruns = s_underruns;
  stats->volume_percent = (uint8_t)s_volume_pct;
  stats->hw_volume = s_hw_volume;
}
