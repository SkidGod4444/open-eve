#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ================= CONFIG =================
const char* WIFI_SSID = "GALGOTIAS-ARUBA";
const char* WIFI_PASS = "1234567@";
static const char* BACKEND_HOST = "backend.dev1974sai.workers.dev";

// ===== MIC (I2S0) =====
#define I2S_WS    15
#define I2S_SCK   16
#define I2S_SD    4

// ===== SPEAKER (I2S1) =====
#define I2S_OUT_WS   17
#define I2S_OUT_SCK  18
#define I2S_OUT_SD   21

#define SAMPLE_RATE_MIC 16000
#define SAMPLE_RATE_OUT 24000

#define BUFFER_SIZE 1024

#ifndef MIC_SAMPLE_RIGHT_SHIFT
/** I2S RX is 32-bit slot; lower value = hotter mic (catch quiet speech). Try 13–15. */
#define MIC_SAMPLE_RIGHT_SHIFT 13
#endif

#ifndef STT_MAX_CAPTURE_SAMPLES
/** Upper bound @ 16 kHz (~3.5 s). Keeps malloc size sane on ESP32-S3 with WiFi+TLS. */
#define STT_MAX_CAPTURE_SAMPLES 56000
#endif
#ifndef STT_MIN_CAPTURE_SAMPLES
/** Require at least this much audio before silence can end capture (avoid cutting first word). */
#define STT_MIN_CAPTURE_SAMPLES 14000
#endif
#ifndef STT_END_SILENCE_MS
/** Stop recording after this many ms below STT_END_SILENCE_PEAK (end of utterance). */
#define STT_END_SILENCE_MS 850
#endif
#ifndef STT_END_SILENCE_PEAK
/** Chunk peak below this counts as silence for endpoint (tune vs room noise). */
#define STT_END_SILENCE_PEAK 720
#endif

#ifndef MIC_PREFILL_DISCARD_SAMPLES
/** Discard after VAD trigger to flush stale FIFO samples before STT buffer. */
#define MIC_PREFILL_DISCARD_SAMPLES 1024
#endif

#ifndef CHAT_HTTP_TIMEOUT_MS
#define CHAT_HTTP_TIMEOUT_MS 180000
#endif
#ifndef STT_HTTP_TIMEOUT_MS
#define STT_HTTP_TIMEOUT_MS 60000
#endif

#ifndef PCM_STREAM_STALL_MS
#define PCM_STREAM_STALL_MS 8000
#endif

#ifndef VAD_PEAK_THRESHOLD
#define VAD_PEAK_THRESHOLD 1900
#endif

/** Max PCM body for /chat buffered mode — heap; lower if Worker TLS fails OOM on S3. */
#ifndef CHAT_PCM_MAX_BYTES
#define CHAT_PCM_MAX_BYTES (448 * 1024)
#endif

/**
 * 0 = Wait for full download, normalize once, play once — recommended (fewer echo/VAD issues).
 * 1 = Stream with jitter prefill — faster first sound but easier to get mic↔speaker feedback.
 */
#ifndef CHAT_PCM_PLAY_MODE
#define CHAT_PCM_PLAY_MODE 0
#endif

#if CHAT_PCM_PLAY_MODE != 0
#ifndef PCM_JITTER_BYTES
#define PCM_JITTER_BYTES 32768
#endif
#else
/** Buffered playback mode: jitter ring unused — tiny placeholder saves ~53 KB SRAM for WiFi. */
#ifndef PCM_JITTER_BYTES
#define PCM_JITTER_BYTES 4096
#endif
#endif
/** Stream mode: minimum queued PCM before first I2S write (~279 ms @ 22.05 kHz when 6144 B). */
#ifndef PCM_STREAM_PREFILL_BYTES
#define PCM_STREAM_PREFILL_BYTES 6144
#endif
#ifndef PCM_STREAM_DRAIN_BLOCK_BYTES
#define PCM_STREAM_DRAIN_BLOCK_BYTES 4096
#endif

// After playback finishes, mic stays off for this long (echo guard). Not additive with clip length.
#ifndef VAD_AFTER_PLAY_PADDING_MS
#define VAD_AFTER_PLAY_PADDING_MS 3200
#endif
#ifndef VAD_COOLDOWN_EMPTY_MS
#define VAD_COOLDOWN_EMPTY_MS 1800
#endif
#ifndef VAD_COOLDOWN_MIN_MS
#define VAD_COOLDOWN_MIN_MS 2200
#endif
/** Extra mute beyond padding for long replies: min(audio_ms/8, cap); avoids doubling clip duration in cooldown. */
#ifndef VAD_LONG_PLAY_EXTRA_CAP_MS
#define VAD_LONG_PLAY_EXTRA_CAP_MS 1600
#endif

/** Full-buffer playback (mode 0): entire HTTP body, then ONE I2S pass.
 * Stream mode (1): jitter ring — still one continuous utterance, not “packet speech”. */

#ifndef PCM_NORMALIZE_PEAK_TARGET
/** Peak-normalize clip to this before boost / I2S (32767 = max int16). */
#define PCM_NORMALIZE_PEAK_TARGET 32767
#endif
#ifndef PCM_NORMALIZE_NOISE_FLOOR
/** Below this abs peak in clip, skip normalization (quiet TTS still scales if above floor). */
#define PCM_NORMALIZE_NOISE_FLOOR 22
#endif

/** Extra digital push after normalize (buffered mode): lifts RMS; peaks clip — louder but hotter. */
#ifndef PCM_SATURATE_BOOST_PERCENT
#define PCM_SATURATE_BOOST_PERCENT 132
#endif

/** I2S write gain: buffered path uses lower value because saturate boost already ran. */
#ifndef BUFFERED_PLAYBACK_GAIN_PERCENT
#define BUFFERED_PLAYBACK_GAIN_PERCENT 118
#endif
#ifndef STREAM_PLAYBACK_GAIN_PERCENT
#define STREAM_PLAYBACK_GAIN_PERCENT 680
#endif

static unsigned g_pcmWriteGainPct = STREAM_PLAYBACK_GAIN_PERCENT;

// Must match setupI2SSpeaker() — used to flush real silence through TX DMA (stops last-sample “stuck”).
#ifndef I2S_SPEAKER_DMA_BUF_COUNT
#define I2S_SPEAKER_DMA_BUF_COUNT 24
#endif
#ifndef I2S_SPEAKER_DMA_BUF_LEN
#define I2S_SPEAKER_DMA_BUF_LEN 512
#endif

int32_t rawBuffer[BUFFER_SIZE];
static int32_t txSlots[BUFFER_SIZE];

/** Heap allocation after WiFi — avoids reserving ~112 KB BSS before WiFi.begin() (ESP32-S3 OOM/crash). */
static int16_t* g_sttCapturePcm = nullptr;

static bool allocSttCaptureBuffer() {
  if (g_sttCapturePcm)
    return true;
  size_t nb = (size_t)STT_MAX_CAPTURE_SAMPLES * sizeof(int16_t);
  g_sttCapturePcm = (int16_t*)malloc(nb);
  if (!g_sttCapturePcm) {
    Serial.printf("[ERR] STT malloc %u fail heap=%u\n", (unsigned)nb,
                  (unsigned)ESP.getFreeHeap());
    return false;
  }
  Serial.printf("[STT] capture heap OK %u B free=%u\n", (unsigned)nb,
                (unsigned)ESP.getFreeHeap());
  return true;
}

// ================= I2S MIC =================
void setupI2SMic() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE_MIC,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("[I2S MIC] ready");
}

// ================= I2S SPEAKER =================
void setupI2SSpeaker() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE_OUT,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_SPEAKER_DMA_BUF_COUNT,
    .dma_buf_len = I2S_SPEAKER_DMA_BUF_LEN
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_OUT_SCK,
    .ws_io_num = I2S_OUT_WS,
    .data_out_num = I2S_OUT_SD,
    .data_in_num = -1
  };

  i2s_driver_install(I2S_NUM_1, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);

  Serial.println("[I2S SPEAKER] ready (24000 Hz — match Worker x-pcm-sample-rate)");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    yield();
  }
  Serial.println("\n[WiFi] connected");
}

void readMic(int16_t* out, int samples) {
  size_t bytesRead;
  i2s_read(I2S_NUM_0, rawBuffer, samples * 4, &bytesRead, portMAX_DELAY);
  for (int i = 0; i < samples; i++) {
    int32_t s = rawBuffer[i] >> MIC_SAMPLE_RIGHT_SHIFT;
    if (s > 32767)
      s = 32767;
    else if (s < -32768)
      s = -32768;
    out[i] = (int16_t)s;
  }
}

/** Peak absolute sample in int16 buffer (chunk). */
static int peakAbsChunk(const int16_t* buf, int samples) {
  int peak = 0;
  for (int i = 0; i < samples; i++) {
    int v = abs(buf[i]);
    if (v > peak) peak = v;
  }
  return peak;
}

// ================= STT (HTTPClient OK for short JSON body) =================
String sendSTT(int16_t* audio, int samples) {
  WiFiClientSecure tls;
  tls.setInsecure();

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(STT_HTTP_TIMEOUT_MS);

  String path = String("/transcribe?sample_rate=") + String(SAMPLE_RATE_MIC) +
                "&channels=1&bits_per_sample=16";
  if (!http.begin(tls, BACKEND_HOST, 443, path.c_str(), true)) {
    Serial.println("[STT] http.begin failed");
    return "";
  }

  http.addHeader("Content-Type", "application/octet-stream");

  int code = http.POST((uint8_t*)audio, samples * 2);

  String res = "";
  if (code > 0) {
    res = http.getString();
  } else {
    Serial.println("[STT] failed");
  }

  http.end();
  return res;
}

static String jsonStringFieldAfterKey(const String& json, const char* keyQuoted) {
  int start = json.indexOf(keyQuoted);
  if (start < 0) return "";
  start += strlen(keyQuoted);
  String out = "";
  for (unsigned i = (unsigned)start; i < (unsigned)json.length(); i++) {
    char c = json.charAt((int)i);
    if (c == '\\' && i + 1 < (unsigned)json.length()) {
      i++;
      char n = json.charAt((int)i);
      if (n == '"')
        out += '"';
      else if (n == '\\')
        out += '\\';
      else if (n == 'n')
        out += '\n';
      else if (n == 'r')
        out += '\r';
      else if (n == 't')
        out += '\t';
      else {
        out += '\\';
        out += n;
      }
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

static String transcriptFromSttResponse(const String& sttJson) {
  return jsonStringFieldAfterKey(sttJson, "\"transcript\":\"");
}

static String languageFromSttResponse(const String& sttJson) {
  return jsonStringFieldAfterKey(sttJson, "\"language_code\":\"");
}

static bool jsonEscapeInto(String& out, const char* s) {
  out = "";
  if (!out.reserve(strlen(s) + 16)) return false;
  for (const char* p = s; *p; p++) {
    char c = *p;
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return true;
}

static String deviceIdFromMac() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[13];
  snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(id);
}

// ----- PCM accumulation (/chat body) then single playback pass -----

static uint8_t* g_pcmAccum = nullptr;
static size_t g_pcmAccumCap = 0;
static size_t g_pcmAccumLen = 0;

static void pcmAccumClear() {
  free(g_pcmAccum);
  g_pcmAccum = nullptr;
  g_pcmAccumCap = 0;
  g_pcmAccumLen = 0;
}

static bool pcmAccumReserve(size_t goal) {
  if (goal > CHAT_PCM_MAX_BYTES)
    goal = CHAT_PCM_MAX_BYTES;
  if (goal <= g_pcmAccumCap)
    return true;
  uint8_t* p = (uint8_t*)realloc(g_pcmAccum, goal);
  if (!p)
    return false;
  g_pcmAccum = p;
  g_pcmAccumCap = goal;
  return true;
}

static bool pcmAccumAppend(const uint8_t* src, size_t n) {
  if (n == 0)
    return true;
  if (g_pcmAccumLen > CHAT_PCM_MAX_BYTES - n)
    return false;
  size_t need = g_pcmAccumLen + n;
  if (need > g_pcmAccumCap) {
    size_t cap = g_pcmAccumCap ? g_pcmAccumCap : 8192;
    while (cap < need && cap < CHAT_PCM_MAX_BYTES)
      cap *= 2;
    if (cap < need)
      cap = need;
    if (cap > CHAT_PCM_MAX_BYTES)
      cap = CHAT_PCM_MAX_BYTES;
    uint8_t* p = (uint8_t*)realloc(g_pcmAccum, cap);
    if (!p)
      return false;
    g_pcmAccum = p;
    g_pcmAccumCap = cap;
  }
  memcpy(g_pcmAccum + g_pcmAccumLen, src, n);
  g_pcmAccumLen += n;
  return true;
}

static uint32_t readU32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/** Byte offset of PCM samples if body is WAV; 0 if raw PCM. */
static size_t pcmWavDataOffset(const uint8_t* d, size_t len) {
  if (len < 36 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
    return 0;
  size_t pos = 12;
  while (pos + 8 <= len) {
    uint32_t chunkSize = readU32le(d + pos + 4);
    if (memcmp(d + pos, "data", 4) == 0)
      return pos + 8;
    size_t next = pos + 8 + (size_t)chunkSize;
    if (chunkSize & 1)
      next++;
    if (next < pos + 8 || next > len)
      break;
    pos = next;
  }
  return 0;
}

/** Peak absolute sample in s[0..n). */
static int32_t pcmS16PeakAbs(const int16_t* s, size_t nSamp) {
  int32_t peak = 0;
  for (size_t i = 0; i < nSamp; i++) {
    int32_t v = (int32_t)s[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  return peak;
}

/**
 * Scale buffered PCM so the loudest sample hits PCM_NORMALIZE_PEAK_TARGET (max headroom use).
 * Modifies samples in place; call once on the full clip before playback.
 */
static void normalizePcmS16FullScale(int16_t* s, size_t nSamp) {
  if (nSamp == 0) return;
  int32_t peak = pcmS16PeakAbs(s, nSamp);
  if (peak < (int32_t)PCM_NORMALIZE_NOISE_FLOOR) {
    Serial.println("[PCM] normalize skipped (near silence)");
    return;
  }
  int64_t scaleQ16 =
      ((int64_t)PCM_NORMALIZE_PEAK_TARGET * 65536LL) / (int64_t)peak;
  for (size_t i = 0; i < nSamp; i++) {
    int64_t x = ((int64_t)s[i] * scaleQ16) >> 16;
    if (x > 32767)
      x = 32767;
    else if (x < -32768)
      x = -32768;
    s[i] = (int16_t)x;
  }
  Serial.printf("[PCM] normalized abs_peak=%ld → target=%d (single pass playback)\n",
                (long)peak, PCM_NORMALIZE_PEAK_TARGET);
}

static void pcmSaturateBoostInPlace(int16_t* s, size_t nSamp, unsigned pct) {
  if (nSamp == 0 || pct == 100)
    return;
  for (size_t i = 0; i < nSamp; i++) {
    int64_t x = ((int64_t)s[i] * (int64_t)pct) / 100;
    if (x > 32767)
      x = 32767;
    else if (x < -32768)
      x = -32768;
    s[i] = (int16_t)x;
  }
}

/** Drain entire PCM buffer to I2S in slices — never truncate after txSlots samples. */
static void writePcmBytesToI2s(uint8_t* buf, size_t len, bool* checkedWav) {
  if (len & 1) len--;

  while (len >= 2) {
    if (!(*checkedWav)) {
      *checkedWav = true;
      if (len >= 4 && memcmp(buf, "RIFF", 4) == 0)
        Serial.println("[PCM] WARN: RIFF header (expect raw PCM)");
    }

    size_t nSamples = len / 2;
    size_t maxSamp = sizeof(txSlots) / sizeof(txSlots[0]);
    size_t chunkSamples = nSamples > maxSamp ? maxSamp : nSamples;

    const int16_t* ps = (const int16_t*)buf;
    for (size_t i = 0; i < chunkSamples; i++) {
      int64_t g =
          ((int64_t)ps[i] * (int64_t)g_pcmWriteGainPct) / 100;
      if (g > 32767)
        g = 32767;
      else if (g < -32768)
        g = -32768;
      txSlots[i] = ((int32_t)g) << 16;
    }

    size_t written = 0;
    i2s_write(I2S_NUM_1, txSlots, chunkSamples * sizeof(int32_t), &written, portMAX_DELAY);
    yield();

    buf += chunkSamples * 2;
    len -= chunkSamples * 2;
  }
}

/** Push zeros through I2S TX until DMA ring is flushed — avoids repeating last PCM forever. */
static void silenceSpeakerTail() {
  memset(txSlots, 0, BUFFER_SIZE * sizeof(int32_t));
  unsigned dmaSamples =
      (unsigned)(I2S_SPEAKER_DMA_BUF_COUNT * I2S_SPEAKER_DMA_BUF_LEN);
  unsigned chunks =
      (dmaSamples + (unsigned)BUFFER_SIZE - 1) / (unsigned)BUFFER_SIZE + 6;
  for (unsigned i = 0; i < chunks; i++) {
    size_t written = 0;
    i2s_write(I2S_NUM_1, txSlots, BUFFER_SIZE * sizeof(int32_t), &written,
               portMAX_DELAY);
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
}

// ----- Stream mode (CHAT_PCM_PLAY_MODE 1): small prefill then play while downloading -----

static uint8_t pcmJb[PCM_JITTER_BYTES];
static size_t pcmJbLen = 0;

static void pcmJbReset() {
  pcmJbLen = 0;
}

static void pcmJbDrain(bool* checkedWav, bool flushAll) {
  if (!flushAll && pcmJbLen < PCM_STREAM_PREFILL_BYTES)
    return;

  while (pcmJbLen >= PCM_STREAM_DRAIN_BLOCK_BYTES ||
         (flushAll && pcmJbLen >= 2)) {
    size_t take = (pcmJbLen >= PCM_STREAM_DRAIN_BLOCK_BYTES)
                      ? PCM_STREAM_DRAIN_BLOCK_BYTES
                      : (pcmJbLen & ~(size_t)1);
    if (take < 2) break;
    writePcmBytesToI2s(pcmJb, take, checkedWav);
    memmove(pcmJb, pcmJb + take, pcmJbLen - take);
    pcmJbLen -= take;
  }
}

static void pcmJbPush(const uint8_t* data, size_t n, bool* checkedWav) {
  while (n > 0) {
    size_t space = sizeof(pcmJb) - pcmJbLen;
    if (space == 0) {
      pcmJbDrain(checkedWav, true);
      space = sizeof(pcmJb) - pcmJbLen;
      if (space == 0) break;
    }
    size_t cpy = n < space ? n : space;
    memcpy(pcmJb + pcmJbLen, data, cpy);
    pcmJbLen += cpy;
    data += cpy;
    n -= cpy;
    pcmJbDrain(checkedWav, false);
  }
}

static void pcmJbFlush(bool* checkedWav) {
  pcmJbDrain(checkedWav, true);
  if (pcmJbLen & 1)
    pcmJbLen--;
}

static void finishStreamPlayback(bool* checkedWav) {
  pcmJbFlush(checkedWav);
  silenceSpeakerTail();
}

/** One contiguous playback pass + DMA silence tail; frees accumulator. */
static size_t playBufferedPcmAndSilence() {
  bool checkedWav = false;
  size_t playLen = 0;
  uint8_t* playPtr = nullptr;

  if (g_pcmAccumLen == 0 || !g_pcmAccum) {
    silenceSpeakerTail();
    pcmAccumClear();
    return 0;
  }

  size_t off = pcmWavDataOffset(g_pcmAccum, g_pcmAccumLen);
  if (off > 0)
    Serial.printf("[PCM] WAV container, data @ %u\n", (unsigned)off);

  playPtr = g_pcmAccum + off;
  playLen = g_pcmAccumLen > off ? g_pcmAccumLen - off : 0;
  playLen &= ~(size_t)1;

  Serial.printf("[PCM] collected %u bytes → one-shot play %u B PCM\n",
                (unsigned)g_pcmAccumLen, (unsigned)playLen);

  i2s_zero_dma_buffer(I2S_NUM_1);
  if (playLen >= 2) {
    normalizePcmS16FullScale((int16_t*)playPtr, playLen / 2);
    pcmSaturateBoostInPlace((int16_t*)playPtr, playLen / 2, PCM_SATURATE_BOOST_PERCENT);
  }

  writePcmBytesToI2s(playPtr, playLen, &checkedWav);
  silenceSpeakerTail();
  pcmAccumClear();
  return playLen;
}

// ----- /chat: raw TLS — ESP32 HTTPClient often leaves chunked PCM unreadable -----

static bool readExact(WiFiClient& c, uint8_t* dst, size_t n, unsigned long deadlineMs) {
  size_t got = 0;
  while (got < n && millis() < deadlineMs) {
    int avail = c.available();
    if (avail > 0) {
      size_t take = (size_t)avail;
      if (take > n - got) take = n - got;
      int r = c.read(dst + got, take);
      if (r > 0) got += (size_t)r;
      yield();
      continue;
    }
    if (!c.connected())
      break;
    delay(1);
    yield();
  }
  return got == n;
}

static bool readLineCrLf(WiFiClient& c, char* line, size_t lineSz, unsigned long deadlineMs) {
  size_t i = 0;
  while (millis() < deadlineMs) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') {
        if (i > 0 && line[i - 1] == '\r')
          i--;
        line[i] = '\0';
        return true;
      }
      if (i + 1 < lineSz)
        line[i++] = ch;
      yield();
    }
    if (!c.connected())
      break;
    delay(1);
    yield();
  }
  line[i] = '\0';
  return false;
}

static bool headersHaveChunked(const String& h) {
  String low = h;
  low.toLowerCase();
  int p = low.indexOf("transfer-encoding:");
  if (p < 0) return false;
  return low.indexOf("chunked", (size_t)p) >= 0;
}

static long parseContentLengthHdr(const String& h) {
  String low = h;
  low.toLowerCase();
  int p = low.indexOf("content-length:");
  if (p < 0) return -1;
  p += 15;
  while (p < (int)low.length() && (low.charAt(p) == ' ' || low.charAt(p) == '\t')) p++;
  int end = p;
  while (end < (int)low.length() && low.charAt(end) >= '0' && low.charAt(end) <= '9') end++;
  if (end == p) return -1;
  return low.substring(p, end).toInt();
}

static int parseHttpStatus(const String& headers) {
  int sp = headers.indexOf(' ');
  if (sp < 0) return -1;
  int sp2 = headers.indexOf(' ', sp + 1);
  if (sp2 < 0) return -1;
  return headers.substring(sp + 1, sp2).toInt();
}

static bool readResponseHeaders(WiFiClientSecure& c, String& out, unsigned long deadlineMs) {
  out = "";
  out.reserve(2048);
  while (millis() < deadlineMs) {
    while (c.available()) {
      char ch = (char)c.read();
      out += ch;
      if (out.length() >= 4 && out.endsWith("\r\n\r\n"))
        return true;
      if (out.length() > 16384)
        return false;
      yield();
    }
    if (!c.connected() && !c.available())
      break;
    delay(1);
    yield();
  }
  return false;
}

/** @return Int16 PCM bytes played (even length, after WAV strip), 0 if none */
size_t sendChatAndSpeak(const String& transcript, const String& langHint) {
  String escaped;
  jsonEscapeInto(escaped, transcript.c_str());

  String body;
  body.reserve(escaped.length() + 160);
  body = "{\"deviceId\":\"";
  body += deviceIdFromMac();
  body += "\",\"text\":\"";
  body += escaped;
  body += "\"";
  if (langHint.length() >= 2) {
    body += ",\"language_code\":\"";
    body += langHint;
    body += "\"";
  }
  body += "}";

  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("[AI] TLS connect…");
  if (!client.connect(BACKEND_HOST, 443)) {
    Serial.println("[AI] TLS connect failed");
    return 0;
  }

  String req = String("POST /chat HTTP/1.1\r\n") +
               "Host: " + BACKEND_HOST + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Accept: application/octet-stream\r\n" +
               "Connection: close\r\n" +
               "Content-Length: " + String(body.length()) + "\r\n" +
               "\r\n" +
               body;

  client.print(req);
  client.flush();

  unsigned long hdrDeadline = millis() + CHAT_HTTP_TIMEOUT_MS;
  String headers;
  if (!readResponseHeaders(client, headers, hdrDeadline)) {
    Serial.println("[AI] headers timeout");
    client.stop();
    return 0;
  }

  int status = parseHttpStatus(headers);
  Serial.printf("[AI] HTTP %d\n", status);

  if (status != 200) {
    unsigned long tEnd = millis() + 8000;
    String err;
    while (millis() < tEnd && client.available()) {
      err += (char)client.read();
      if (err.length() > 600) break;
      yield();
    }
    Serial.println(err.length() ? err : "(no body)");
    client.stop();
    return 0;
  }

  bool chunked = headersHaveChunked(headers);
  long contentLen = parseContentLengthHdr(headers);
  Serial.printf("[AI] chunked=%d content-length=%ld\n", chunked ? 1 : 0, contentLen);

  const bool streamPlay = (CHAT_PCM_PLAY_MODE != 0);
  g_pcmWriteGainPct =
      streamPlay ? STREAM_PLAYBACK_GAIN_PERCENT : BUFFERED_PLAYBACK_GAIN_PERCENT;

  if (!streamPlay) {
    pcmAccumClear();
    if (!chunked && contentLen > 0 &&
        (unsigned long)contentLen <= (unsigned long)CHAT_PCM_MAX_BYTES)
      pcmAccumReserve((size_t)contentLen);
  } else {
    pcmJbReset();
    i2s_zero_dma_buffer(I2S_NUM_1);
    Serial.printf("[PCM] stream mode (prefill %u B)\n",
                  (unsigned)PCM_STREAM_PREFILL_BYTES);
  }

  uint8_t buf[1024];
  size_t totalRx = 0;
  bool accumFull = false;
  bool checkedWav = false;
  unsigned long pcmDeadline = millis() + CHAT_HTTP_TIMEOUT_MS;

  if (chunked) {
    for (;;) {
      if (!streamPlay && accumFull)
        break;
      char lineBuf[96];
      if (!readLineCrLf(client, lineBuf, sizeof(lineBuf), pcmDeadline)) {
        Serial.println("[PCM] chunk-size line timeout");
        break;
      }

      String szLine = String(lineBuf);
      szLine.trim();
      int semi = szLine.indexOf(';');
      if (semi >= 0) szLine = szLine.substring(0, semi);
      szLine.trim();

      unsigned long chunkSz = strtoul(szLine.c_str(), nullptr, 16);
      if (chunkSz == 0) {
        while (millis() < pcmDeadline) {
          if (!readLineCrLf(client, lineBuf, sizeof(lineBuf), millis() + 8000))
            break;
          if (lineBuf[0] == '\0')
            break;
        }
        break;
      }

      while (chunkSz > 0) {
        size_t take = chunkSz > sizeof(buf) ? sizeof(buf) : (size_t)chunkSz;
        if (!readExact(client, buf, take, pcmDeadline)) {
          Serial.println("[PCM] short chunk read");
          client.stop();
          if (streamPlay) {
            finishStreamPlayback(&checkedWav);
            size_t played = totalRx & ~(size_t)1;
            Serial.printf("[AUDIO DONE] rx=%u played=%u (stream truncated)\n",
                          (unsigned)totalRx, (unsigned)played);
            return played;
          }
          size_t played = playBufferedPcmAndSilence();
          Serial.printf("[AUDIO DONE] rx=%u played=%u (truncated)\n", (unsigned)totalRx,
                        (unsigned)played);
          return played;
        }
        if (streamPlay)
          pcmJbPush(buf, take, &checkedWav);
        else if (!pcmAccumAppend(buf, take)) {
          Serial.println("[PCM] buffer limit — draining chunk then stopping capture");
          accumFull = true;
          chunkSz -= take;
          while (chunkSz > 0) {
            size_t dt = chunkSz > sizeof(buf) ? sizeof(buf) : chunkSz;
            if (!readExact(client, buf, dt, pcmDeadline))
              break;
            chunkSz -= dt;
          }
          break;
        }
        totalRx += take;
        chunkSz -= take;
      }

      if (!streamPlay && accumFull)
        break;

      uint8_t crlf[2];
      if (!readExact(client, crlf, 2, millis() + 8000)) {
        Serial.println("[PCM] missing CRLF after chunk");
        break;
      }
    }
  } else if (contentLen > 0) {
    while (contentLen > 0 && (!streamPlay ? !accumFull : true)) {
      size_t take = (size_t)contentLen > sizeof(buf) ? sizeof(buf) : (size_t)contentLen;
      if (!readExact(client, buf, take, pcmDeadline))
        break;
      if (streamPlay)
        pcmJbPush(buf, take, &checkedWav);
      else if (!pcmAccumAppend(buf, take)) {
        Serial.println("[PCM] buffer limit");
        accumFull = true;
        contentLen -= (long)take;
        while (contentLen > 0) {
          size_t dt =
              (size_t)contentLen > sizeof(buf) ? sizeof(buf) : (size_t)contentLen;
          if (!readExact(client, buf, dt, pcmDeadline))
            break;
          contentLen -= (long)dt;
        }
        break;
      }
      totalRx += take;
      contentLen -= (long)take;
    }
  } else {
    unsigned long lastData = millis();
    while (millis() < pcmDeadline && (!streamPlay ? !accumFull : true)) {
      int avail = client.available();
      if (avail > 0) {
        lastData = millis();
        int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
        if (want & 1) want--;
        int len = client.read(buf, want > 0 ? want : 0);
        if (len > 0) {
          if (streamPlay)
            pcmJbPush(buf, (size_t)len, &checkedWav);
          else if (!pcmAccumAppend(buf, (size_t)len)) {
            Serial.println("[PCM] buffer limit");
            accumFull = true;
            break;
          }
          totalRx += (size_t)len;
        }
        yield();
        continue;
      }
      if (!client.connected())
        break;
      if (millis() - lastData > PCM_STREAM_STALL_MS)
        break;
      delay(1);
      yield();
    }
  }

  client.stop();
  if (streamPlay) {
    finishStreamPlayback(&checkedWav);
    size_t played = totalRx & ~(size_t)1;
    Serial.printf("[AUDIO DONE] rx=%u stream PCM bytes (no whole-buffer wait)\n",
                  (unsigned)totalRx);
    return played;
  }
  size_t played = playBufferedPcmAndSilence();
  Serial.printf("[AUDIO DONE] rx=%u played=%u PCM bytes\n", (unsigned)totalRx, (unsigned)played);
  return played;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[BOOT]");
  Serial.printf("[BOOT] heap=%u\n", (unsigned)ESP.getFreeHeap());
  connectWiFi();
  Serial.printf("[BOOT] heap after WiFi=%u\n", (unsigned)ESP.getFreeHeap());
  if (!allocSttCaptureBuffer()) {
    Serial.println("[HALT] Out of RAM for voice capture buffer");
    while (true) {
      delay(1000);
      yield();
    }
  }
  setupI2SMic();
  setupI2SSpeaker();
  Serial.printf("[READY] heap=%u\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  static int16_t audio[BUFFER_SIZE];
  static unsigned long vadCooldownUntilMs = 0;

  unsigned long now = millis();
  if (now < vadCooldownUntilMs) {
    delay(15);
    return;
  }

  readMic(audio, BUFFER_SIZE);

  int peak = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    peak = max(peak, abs(audio[i]));
  }

  if (peak > VAD_PEAK_THRESHOLD) {
    if (!g_sttCapturePcm) {
      if (!allocSttCaptureBuffer()) {
        vadCooldownUntilMs = millis() + 3000;
        return;
      }
    }

    Serial.println("[VOICE DETECTED]");

    int disc = 0;
    while (disc < MIC_PREFILL_DISCARD_SAMPLES) {
      readMic(audio, BUFFER_SIZE);
      disc += BUFFER_SIZE;
    }

    size_t totalWritten = 0;
    unsigned silentRunSamples = 0;
    const unsigned silentNeedSamples =
        (unsigned)SAMPLE_RATE_MIC * (unsigned)STT_END_SILENCE_MS / 1000U;

    while (totalWritten < (size_t)STT_MAX_CAPTURE_SAMPLES) {
      size_t chunk = BUFFER_SIZE;
      if (totalWritten + chunk > (size_t)STT_MAX_CAPTURE_SAMPLES)
        chunk = (size_t)STT_MAX_CAPTURE_SAMPLES - totalWritten;
      if (chunk == 0)
        break;

      readMic(g_sttCapturePcm + totalWritten, (int)chunk);
      totalWritten += chunk;

      int pk = peakAbsChunk(g_sttCapturePcm + totalWritten - chunk, (int)chunk);
      if (pk < STT_END_SILENCE_PEAK)
        silentRunSamples += (unsigned)chunk;
      else
        silentRunSamples = 0;

      if (totalWritten >= (size_t)STT_MIN_CAPTURE_SAMPLES &&
          silentRunSamples >= silentNeedSamples)
        break;
    }

    Serial.printf("[STT] captured %u samples (~%lu ms @ %u Hz)\n",
                  (unsigned)totalWritten,
                  (unsigned long)((totalWritten * 1000UL) / (unsigned long)SAMPLE_RATE_MIC),
                  (unsigned)SAMPLE_RATE_MIC);

    String stt = sendSTT(g_sttCapturePcm, (int)totalWritten);
    Serial.println("[STT] " + stt);

    String transcript = transcriptFromSttResponse(stt);
    String langHint = languageFromSttResponse(stt);
    transcript.trim();
    Serial.println("[YOU] " + transcript);

    if (transcript.length() > 0) {
      size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
      pcmBytes &= ~(size_t)1;
      unsigned long audioMs =
          pcmBytes > 0 ? ((pcmBytes / 2) * 1000UL / (unsigned long)SAMPLE_RATE_OUT) : 0;
      /* Playback already ran for audioMs; do not add full audioMs again or mute doubles clip length. */
      unsigned long extraLong =
          audioMs / 8UL > (unsigned long)VAD_LONG_PLAY_EXTRA_CAP_MS
              ? (unsigned long)VAD_LONG_PLAY_EXTRA_CAP_MS
              : audioMs / 8UL;
      unsigned long guard =
          (unsigned long)VAD_AFTER_PLAY_PADDING_MS + extraLong;
      if (guard < (unsigned long)VAD_COOLDOWN_MIN_MS)
        guard = (unsigned long)VAD_COOLDOWN_MIN_MS;
      if (guard > 90000UL)
        guard = 90000UL;
      vadCooldownUntilMs = millis() + guard;
      Serial.printf("[VAD] mute-after-play %lu ms (spoken ~%lu ms; tail blocks echo)\n", guard,
                    audioMs);
    } else {
      vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
    }
  }
}
