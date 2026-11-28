/**
 * Web radio player: HTTP(S) Icecast stream -> MP3/AAC decode -> USB DAC.
 *
 * Two tasks connected by a byte buffer:
 *
 *   net_task  reads the HTTP body, strips ICY metadata, pushes audio bytes
 *   dec_task  pops bytes, decodes to PCM, resamples to the USB rate if needed,
 *             and writes to the USB output FIFO
 *
 * audio_output_write() never blocks, so dec_task paces itself against the FIFO
 * fill level.  That stalls the byte buffer, which stalls TCP, so the server is
 * read at playback speed.
 */

#include "radio_player.h"

#include "audio_output.h"
#include "audio_resample.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "stations.h"
#include "wifi_manager.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "radio"

#define OUT_RATE CONFIG_OUTPUT_SAMPLE_RATE_HZ

#define BUFFER_BYTES    ((size_t)CONFIG_RADIO_BUFFER_KB * 1024)
#define PREBUFFER_BYTES ((size_t)CONFIG_RADIO_PREBUFFER_KB * 1024)

#if CONFIG_RADIO_PREBUFFER_KB >= CONFIG_RADIO_BUFFER_KB
#error "CONFIG_RADIO_PREBUFFER_KB must be smaller than CONFIG_RADIO_BUFFER_KB"
#endif

#define NET_CHUNK_BYTES 2048
#define DEC_CHUNK_BYTES 2048
#define MAX_REDIRECTS   5
#define MAX_EAGAIN      3 /* consecutive read timeouts before reconnecting */
#define SLICE_FRAMES    256

/* The only resampler available is compiled out when the output is 44.1 kHz. */
#define CAN_RESAMPLE (OUT_RATE != 44100)

#if CONFIG_FREERTOS_UNICORE
#define NET_CORE 0
#define DEC_CORE 0
#else
#define NET_CORE 0
#define DEC_CORE 1
#endif

static StreamBufferHandle_t s_sb;

/* Incremented by the network task when the decoder must drop everything and
 * start fresh (first start, station change).  The decoder acknowledges by
 * copying it to s_dec_gen. */
static volatile uint32_t s_gen;
static volatile uint32_t s_dec_gen;
/* Incremented by radio_player_select() to make the network task switch. */
static volatile uint32_t s_select_gen;
static volatile size_t s_station;
static volatile esp_audio_simple_dec_type_t s_codec;

static volatile bool s_streaming;
static volatile uint32_t s_reconnects;
static volatile uint32_t s_decode_errors;
static volatile uint32_t s_rate;
static volatile uint8_t s_channels;
static char s_title[96];

/* ── HTTP ────────────────────────────────────────────────────────────────── */

typedef struct {
  char content_type[64];
  int metaint;
} http_info_t;

static esp_err_t http_event(esp_http_client_event_t *evt) {
  http_info_t *info = evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_HEADER && info) {
    if (strcasecmp(evt->header_key, "Content-Type") == 0) {
      strlcpy(info->content_type, evt->header_value,
              sizeof(info->content_type));
    } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
      info->metaint = atoi(evt->header_value);
    }
  }
  return ESP_OK;
}

static esp_audio_simple_dec_type_t codec_from_content_type(const char *ct) {
  if (strcasestr(ct, "aac") != NULL) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  }
  if (ct[0] == '\0' || strcasestr(ct, "mpeg") != NULL ||
      strcasestr(ct, "mp3") != NULL) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3; /* default when unspecified */
  }
  return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* ── ICY metadata ────────────────────────────────────────────────────────── */

/* Icecast interleaves a metadata block every `metaint` audio bytes when asked
 * with "Icy-MetaData: 1".  Layout: metaint audio bytes, 1 length byte (x16),
 * length bytes of text, repeat. */
typedef struct {
  int metaint;   /* 0 = stream carries no metadata */
  int to_meta;   /* audio bytes left before the next metadata block */
  int meta_left; /* metadata bytes left to skip in the current block */
  char meta[256];
  size_t meta_len;
} icy_t;

static void icy_parse_title(icy_t *icy) {
  icy->meta[icy->meta_len < sizeof(icy->meta) ? icy->meta_len
                                              : sizeof(icy->meta) - 1] = '\0';
  const char *start = strstr(icy->meta, "StreamTitle='");
  if (!start) {
    return;
  }
  start += strlen("StreamTitle='");
  const char *end = strstr(start, "';");
  size_t len = end ? (size_t)(end - start) : strlen(start);
  if (len >= sizeof(s_title)) {
    len = sizeof(s_title) - 1;
  }
  if (len == 0 ||
      (strlen(s_title) == len && strncmp(s_title, start, len) == 0)) {
    return;
  }
  memcpy(s_title, start, len);
  s_title[len] = '\0';
  ESP_LOGI(TAG, "Now playing: %s", s_title);
}

/* Queue bytes for the decoder.  Returns false if the station was changed. */
static bool buffer_send(const uint8_t *data, size_t len, uint32_t sel) {
  while (len > 0) {
    if (sel != s_select_gen) {
      return false;
    }
    size_t sent = xStreamBufferSend(s_sb, data, len, pdMS_TO_TICKS(100));
    data += sent;
    len -= sent;
  }
  return true;
}

/* Split metadata from audio and queue the audio. */
static bool push_audio(icy_t *icy, const uint8_t *data, size_t len,
                       uint32_t sel) {
  while (len > 0) {
    if (icy->meta_left > 0) {
      size_t k = len < (size_t)icy->meta_left ? len : (size_t)icy->meta_left;
      size_t room = sizeof(icy->meta) - 1 - icy->meta_len;
      size_t copy = k < room ? k : room;
      memcpy(icy->meta + icy->meta_len, data, copy);
      icy->meta_len += copy;
      data += k;
      len -= k;
      icy->meta_left -= (int)k;
      if (icy->meta_left == 0) {
        icy_parse_title(icy);
      }
      continue;
    }
    if (icy->metaint > 0 && icy->to_meta == 0) {
      icy->meta_left = data[0] * 16;
      icy->meta_len = 0;
      icy->to_meta = icy->metaint;
      data++;
      len--;
      continue;
    }
    size_t k = len;
    if (icy->metaint > 0 && k > (size_t)icy->to_meta) {
      k = (size_t)icy->to_meta;
    }
    if (!buffer_send(data, k, sel)) {
      return false;
    }
    if (icy->metaint > 0) {
      icy->to_meta -= (int)k;
    }
    data += k;
    len -= k;
  }
  return true;
}

/* Open the station URL, following redirects.  On success the headers have
 * been read and the body is ready for esp_http_client_read(). */
static bool http_open(esp_http_client_handle_t client, http_info_t *info) {
  for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
    memset(info, 0, sizeof(*info));
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Connect failed: %s", esp_err_to_name(err));
      return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 200) {
      return true;
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 ||
        status == 308) {
      esp_http_client_set_redirection(client);
      esp_http_client_close(client);
      continue;
    }
    ESP_LOGW(TAG, "Server answered HTTP %d", status);
    return false;
  }
  ESP_LOGW(TAG, "Too many redirects");
  return false;
}

/* Ask the decoder to drop its state and wait until it has. */
static void flush_decoder(void) {
  uint32_t gen = ++s_gen;
  for (int i = 0; i < 200 && s_dec_gen != gen; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* Stream one station until it fails, ends or the selection changes. */
static void stream_once(size_t idx, uint32_t sel) {
  const radio_station_t *st = &g_stations[idx];
  http_info_t info;
  icy_t icy = {0};

  esp_http_client_config_t cfg = {
      .url = st->url,
      .event_handler = http_event,
      .user_data = &info,
      .timeout_ms = CONFIG_RADIO_HTTP_TIMEOUT_MS,
      .buffer_size = 2048,
      .buffer_size_tx = 1024,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_redirect = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  uint8_t *buf = malloc(NET_CHUNK_BYTES);
  if (!client || !buf) {
    ESP_LOGE(TAG, "Out of memory");
    goto done;
  }
  esp_http_client_set_header(client, "Icy-MetaData", "1");
  esp_http_client_set_header(client, "User-Agent", "esp32-webradio/1.0");

  if (!http_open(client, &info)) {
    goto done;
  }

  esp_audio_simple_dec_type_t codec =
      codec_from_content_type(info.content_type);
  if (codec == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
    ESP_LOGE(TAG, "Unsupported stream type \"%s\" (need MP3 or AAC)",
             info.content_type);
    /* Keep the connection pointless to retry quickly: caller backs off. */
    goto done;
  }
  s_codec = codec;
  icy.metaint = info.metaint;
  icy.to_meta = info.metaint;
  ESP_LOGI(TAG, "Streaming \"%s\" (%s, metadata every %d bytes)", st->name,
           info.content_type[0] ? info.content_type : "unknown type",
           info.metaint);
  s_streaming = true;

  int timeouts = 0;
  while (sel == s_select_gen) {
    int n = esp_http_client_read(client, (char *)buf, NET_CHUNK_BYTES);
    if (n > 0) {
      timeouts = 0;
      if (!push_audio(&icy, buf, (size_t)n, sel)) {
        break;
      }
    } else if (n == 0) {
      ESP_LOGW(TAG, "Server closed the stream");
      break;
    } else if (n == -ESP_ERR_HTTP_EAGAIN) {
      if (++timeouts >= MAX_EAGAIN) {
        ESP_LOGW(TAG, "Stream stalled, reconnecting");
        break;
      }
    } else {
      ESP_LOGW(TAG, "Read error %d, reconnecting", n);
      break;
    }
  }

done:
  s_streaming = false;
  free(buf);
  if (client) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
}

static void net_task(void *arg) {
  (void)arg;
  uint32_t flushed_for = UINT32_MAX;
  uint32_t backoff_ms = 1000;
  bool first = true;

  while (true) {
    if (!wifi_manager_is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint32_t sel = s_select_gen;
    size_t idx = s_station;
    if (sel != flushed_for) {
      /* New station: discard everything from the previous one. */
      flush_decoder();
      s_title[0] = '\0';
      flushed_for = sel;
      backoff_ms = 1000;
    }
    if (!first) {
      s_reconnects++;
    }
    first = false;

    TickType_t started = xTaskGetTickCount();
    stream_once(idx, sel);

    if (sel != s_select_gen) {
      continue; /* switching station: reconnect straight away */
    }
    /* Back off, but start over if the stream had been healthy for a while. */
    if ((xTaskGetTickCount() - started) > pdMS_TO_TICKS(15000)) {
      backoff_ms = 1000;
    }
    vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    if (backoff_ms < 10000) {
      backoff_ms *= 2;
    }
  }
}

/* ── Decoder ─────────────────────────────────────────────────────────────── */

typedef struct {
  esp_audio_simple_dec_handle_t dec;
  esp_audio_simple_dec_type_t type;
  uint8_t *pcm; /* decoder output */
  size_t pcm_cap;
  int16_t *stereo;      /* mono/multichannel -> stereo scratch */
  size_t stereo_cap;    /* frames */
  int16_t *resampled;   /* resampler output */
  size_t resampled_cap; /* frames */
  uint32_t rate;
  uint8_t channels;
  bool format_ok;
  bool resampling;
  uint32_t gen;
} dec_state_t;

static void dec_close(dec_state_t *d) {
  if (d->dec) {
    esp_audio_simple_dec_close(d->dec);
    d->dec = NULL;
  }
  d->rate = 0;
  d->channels = 0;
  d->format_ok = false;
  d->resampling = false;
  s_rate = 0;
  s_channels = 0;
  audio_resample_destroy();
}

static bool dec_open(dec_state_t *d, esp_audio_simple_dec_type_t type) {
  esp_audio_simple_dec_cfg_t cfg = {.dec_type = type};
  esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &d->dec);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "Cannot open %s decoder (%d)",
             esp_audio_simple_dec_get_name(type), err);
    d->dec = NULL;
    return false;
  }
  d->type = type;
  ESP_LOGI(TAG, "%s decoder ready", esp_audio_simple_dec_get_name(type));
  return true;
}

/* Prepare for a stream with this format.  Sets d->format_ok. */
static void dec_set_format(dec_state_t *d, uint32_t rate, uint8_t channels) {
  d->rate = rate;
  d->channels = channels;
  d->format_ok = false;
  d->resampling = false;
  audio_resample_destroy();

  if (rate != OUT_RATE) {
    if (!CAN_RESAMPLE) {
      ESP_LOGE(TAG,
               "Stream is %lu Hz but the USB output is fixed at %d Hz and "
               "resampling is not built in for that rate. Set the USB DAC "
               "sample rate to 48000 in menuconfig.",
               (unsigned long)rate, OUT_RATE);
      return;
    }
    if (!audio_resample_init(rate, OUT_RATE, 2)) {
      ESP_LOGE(TAG, "Resampler init failed (%lu -> %d Hz)", (unsigned long)rate,
               OUT_RATE);
      return;
    }
    size_t cap = (size_t)((double)SLICE_FRAMES * OUT_RATE / rate) + 32;
    if (cap > d->resampled_cap) {
      free(d->resampled);
      d->resampled = malloc(cap * 2 * sizeof(int16_t));
      d->resampled_cap = d->resampled ? cap : 0;
    }
    if (!d->resampled) {
      ESP_LOGE(TAG, "Out of memory for resampler buffer");
      return;
    }
    d->resampling = true;
  }
  d->format_ok = true;
  s_rate = rate;
  s_channels = channels;
  ESP_LOGI(TAG, "Stream: %lu Hz, %u ch%s", (unsigned long)rate, channels,
           d->resampling ? " (resampled)" : "");
}

/* Wait for room in the USB FIFO, then queue.  Returns false if the decoder
 * was told to start over. */
static bool write_paced(const dec_state_t *d, const int16_t *pcm,
                        size_t frames) {
  while (frames > 0) {
    if (s_gen != d->gen) {
      return false;
    }
    audio_output_stats_t st;
    audio_output_get_stats(&st);
    if (!st.connected) {
      /* No DAC: discard at real-time speed so the stream stays live. */
      TickType_t t = pdMS_TO_TICKS((uint64_t)frames * 1000 / OUT_RATE);
      vTaskDelay(t ? t : 1);
      return true;
    }
    size_t free_frames = (st.capacity_bytes - st.fill_bytes) / 4;
    if (free_frames == 0) {
      TickType_t t = pdMS_TO_TICKS(5);
      vTaskDelay(t ? t : 1);
      continue;
    }
    size_t n = frames < free_frames ? frames : free_frames;
    n = audio_output_write(pcm, n);
    pcm += n * 2;
    frames -= n;
  }
  return true;
}

/* Turn decoded PCM into stereo at the USB rate and queue it. */
static bool output_pcm(dec_state_t *d, const int16_t *pcm, size_t frames) {
  while (frames > 0) {
    size_t slice = frames < SLICE_FRAMES ? frames : SLICE_FRAMES;
    const int16_t *src = pcm;

    if (d->channels != 2) {
      for (size_t i = 0; i < slice; i++) {
        int16_t l = pcm[i * d->channels];
        int16_t r = d->channels > 1 ? pcm[i * d->channels + 1] : l;
        d->stereo[i * 2] = l;
        d->stereo[i * 2 + 1] = r;
      }
      src = d->stereo;
    }

    const int16_t *out = src;
    size_t out_frames = slice;
    if (d->resampling) {
      out_frames =
          audio_resample_process(src, slice, d->resampled, d->resampled_cap);
      out = d->resampled;
    }
    if (!write_paced(d, out, out_frames)) {
      return false;
    }
    pcm += slice * d->channels;
    frames -= slice;
  }
  return true;
}

static bool grow_pcm(dec_state_t *d, size_t needed) {
  uint8_t *p = realloc(d->pcm, needed);
  if (!p) {
    ESP_LOGE(TAG, "Out of memory for %u byte decode buffer", (unsigned)needed);
    return false;
  }
  d->pcm = p;
  d->pcm_cap = needed;
  return true;
}

static void decode_chunk(dec_state_t *d, uint8_t *data, size_t len) {
  esp_audio_simple_dec_raw_t raw = {.buffer = data, .len = (uint32_t)len};

  while (raw.len > 0) {
    esp_audio_simple_dec_out_t out = {.buffer = d->pcm,
                                      .len = (uint32_t)d->pcm_cap};
    esp_audio_err_t err = esp_audio_simple_dec_process(d->dec, &raw, &out);
    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      if (!grow_pcm(d, out.needed_size)) {
        return;
      }
      continue;
    }
    if (err != ESP_AUDIO_ERR_OK) {
      /* Bad frame or lost sync (e.g. after a reconnect); drop this chunk, the
       * parser finds the next frame header by itself. */
      s_decode_errors++;
      return;
    }

    if (out.decoded_size > 0) {
      esp_audio_simple_dec_info_t info;
      if (esp_audio_simple_dec_get_info(d->dec, &info) == ESP_AUDIO_ERR_OK &&
          info.channel > 0 &&
          (info.sample_rate != d->rate || info.channel != d->channels)) {
        if (info.bits_per_sample != 16) {
          ESP_LOGE(TAG, "Unsupported %u-bit decoder output",
                   info.bits_per_sample);
          return;
        }
        size_t need = SLICE_FRAMES;
        if (need > d->stereo_cap) {
          free(d->stereo);
          d->stereo = malloc(need * 2 * sizeof(int16_t));
          d->stereo_cap = d->stereo ? need : 0;
        }
        if (!d->stereo) {
          ESP_LOGE(TAG, "Out of memory");
          return;
        }
        dec_set_format(d, info.sample_rate, info.channel);
      }
      if (d->format_ok) {
        size_t frames = out.decoded_size / (2u * d->channels);
        if (!output_pcm(d, (const int16_t *)out.buffer, frames)) {
          return;
        }
      }
    }

    if (raw.consumed == 0 && out.decoded_size == 0) {
      break; /* no progress */
    }
    raw.buffer += raw.consumed;
    raw.len -= raw.consumed;
  }
}

static void dec_task(void *arg) {
  (void)arg;
  esp_audio_dec_register_default();
  esp_audio_simple_dec_register_default();

  dec_state_t d = {0};
  d.pcm_cap = 0;
  if (!grow_pcm(&d, 8192)) {
    vTaskDelete(NULL);
    return;
  }
  uint8_t *in = malloc(DEC_CHUNK_BYTES);
  if (!in) {
    ESP_LOGE(TAG, "Out of memory");
    vTaskDelete(NULL);
    return;
  }

  bool prebuffering = true;

  while (true) {
    if (s_gen != d.gen) {
      /* Fresh stream: drop old decoder, buffered bytes and queued audio. */
      d.gen = s_gen;
      dec_close(&d);
      while (xStreamBufferReceive(s_sb, in, DEC_CHUNK_BYTES, 0) > 0) {
      }
      audio_output_flush();
      prebuffering = true;
      s_dec_gen = d.gen;
      continue;
    }

    if (prebuffering) {
      if (xStreamBufferBytesAvailable(s_sb) >= PREBUFFER_BYTES) {
        prebuffering = false;
        ESP_LOGI(TAG, "Buffered %u KB, starting playback",
                 (unsigned)(PREBUFFER_BYTES / 1024));
      } else {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }

    size_t n =
        xStreamBufferReceive(s_sb, in, DEC_CHUNK_BYTES, pdMS_TO_TICKS(100));
    if (n == 0) {
      continue;
    }

    if (!d.dec && !dec_open(&d, s_codec)) {
      vTaskDelay(pdMS_TO_TICKS(500)); /* drop data, retry on next chunk */
      continue;
    }
    decode_chunk(&d, in, n);
  }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t radio_player_start(void) {
  if (g_station_count == 0) {
    ESP_LOGE(TAG, "No stations configured (see main/radio/stations.c)");
    return ESP_ERR_NOT_FOUND;
  }

  s_sb = xStreamBufferCreateWithCaps(BUFFER_BYTES, 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_sb) {
    s_sb = xStreamBufferCreateWithCaps(BUFFER_BYTES, 1, MALLOC_CAP_8BIT);
  }
  if (!s_sb) {
    ESP_LOGE(TAG, "Cannot allocate %u KB stream buffer",
             (unsigned)(BUFFER_BYTES / 1024));
    return ESP_ERR_NO_MEM;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(dec_task, "radio_dec", 12288, NULL, 6,
                                          NULL, DEC_CORE);
  if (ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ok = xTaskCreatePinnedToCore(net_task, "radio_net", 12288, NULL, 5, NULL,
                               NET_CORE);
  if (ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "Starting with \"%s\"", g_stations[0].name);
  return ESP_OK;
}

void radio_player_select(size_t index) {
  if (index >= g_station_count) {
    return;
  }
  ESP_LOGI(TAG, "Switching to \"%s\"", g_stations[index].name);
  s_station = index;
  s_select_gen++;
}

void radio_player_next(void) {
  radio_player_select((s_station + 1) % g_station_count);
}

void radio_player_get_stats(radio_player_stats_t *stats) {
  if (!stats) {
    return;
  }
  memset(stats, 0, sizeof(*stats));
  stats->station = g_stations[s_station].name;
  stats->streaming = s_streaming;
  esp_audio_simple_dec_type_t codec = s_codec;
  stats->codec = (s_rate && codec != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE)
                     ? esp_audio_simple_dec_get_name(codec)
                     : "-";
  stats->sample_rate = s_rate;
  stats->channels = s_channels;
  stats->buffered_bytes =
      s_sb ? (uint32_t)xStreamBufferBytesAvailable(s_sb) : 0;
  stats->capacity_bytes = (uint32_t)BUFFER_BYTES;
  stats->reconnects = s_reconnects;
  stats->decode_errors = s_decode_errors;
  strlcpy(stats->title, s_title, sizeof(stats->title));
}
