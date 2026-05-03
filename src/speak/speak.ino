#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>
#include <stdlib.h>

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
#define VAD_PEAK_THRESHOLD 2800
#endif

#ifndef PCM_JITTER_BYTES
#define PCM_JITTER_BYTES 32768
#endif
#ifndef PCM_PREFILL_BYTES
#define PCM_PREFILL_BYTES 8192
#endif
#ifndef PCM_DRAIN_BLOCK_BYTES
#define PCM_DRAIN_BLOCK_BYTES 4096
#endif

// Mic picks up speaker → false VAD; stay deaf for (audio length + pad).
#ifndef VAD_AFTER_PLAY_PADDING_MS
#define VAD_AFTER_PLAY_PADDING_MS 4000
#endif
#ifndef VAD_COOLDOWN_EMPTY_MS
#define VAD_COOLDOWN_EMPTY_MS 1500
#endif
#ifndef VAD_COOLDOWN_MIN_MS
#define VAD_COOLDOWN_MIN_MS 3500
#endif

#ifndef PLAYBACK_GAIN_PERCENT
#define PLAYBACK_GAIN_PERCENT 320
#endif

int32_t rawBuffer[BUFFER_SIZE];
static int32_t txSlots[BUFFER_SIZE];

// ================= I2S MIC =================
void setupI2SMic() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE_MIC,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256
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
    .dma_buf_count = 24,
    .dma_buf_len = 512
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

  Serial.println("[I2S SPEAKER] ready (24 kHz, int16 in 32-bit slots)");
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] connected");
}

void readMic(int16_t* out, int samples) {
  size_t bytesRead;
  i2s_read(I2S_NUM_0, rawBuffer, samples * 4, &bytesRead, portMAX_DELAY);
  for (int i = 0; i < samples; i++) {
    out[i] = rawBuffer[i] >> 14;
  }
}

// ================= STT (HTTPClient OK for short JSON body) =================
String sendSTT(int16_t* audio, int samples) {
  WiFiClientSecure tls;
  tls.setInsecure();

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(STT_HTTP_TIMEOUT_MS);

  if (!http.begin(tls, BACKEND_HOST, 443, "/transcribe", true)) {
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

// ----- Playback jitter buffer + slice I2S writes (txSlots size cap) -----

static uint8_t pcmJb[PCM_JITTER_BYTES];
static size_t pcmJbLen = 0;

static void pcmJbReset() {
  pcmJbLen = 0;
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
      int32_t g = ((int32_t)ps[i] * (int32_t)PLAYBACK_GAIN_PERCENT) / 100;
      if (g > 32767) g = 32767;
      if (g < -32768) g = -32768;
      txSlots[i] = g << 16;
    }

    size_t written = 0;
    i2s_write(I2S_NUM_1, txSlots, chunkSamples * sizeof(int32_t), &written, portMAX_DELAY);

    buf += chunkSamples * 2;
    len -= chunkSamples * 2;
  }
}

static void pcmJbDrain(bool* checkedWav, bool flushAll) {
  if (!flushAll && pcmJbLen < PCM_PREFILL_BYTES)
    return;

  while (pcmJbLen >= PCM_DRAIN_BLOCK_BYTES || (flushAll && pcmJbLen >= 2)) {
    size_t take =
        (pcmJbLen >= PCM_DRAIN_BLOCK_BYTES) ? PCM_DRAIN_BLOCK_BYTES : (pcmJbLen & ~(size_t)1);
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

/** @return PCM body bytes decoded to speaker, 0 if no audio played */
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

  i2s_zero_dma_buffer(I2S_NUM_1);
  pcmJbReset();

  uint8_t buf[1024];
  bool checkedWav = false;
  size_t totalBytes = 0;
  unsigned long pcmDeadline = millis() + CHAT_HTTP_TIMEOUT_MS;

  if (chunked) {
    for (;;) {
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
          pcmJbFlush(&checkedWav);
          client.stop();
          Serial.printf("[AUDIO DONE] %u bytes\n", (unsigned)totalBytes);
          return totalBytes;
        }
        pcmJbPush(buf, take, &checkedWav);
        totalBytes += take;
        chunkSz -= take;
      }

      uint8_t crlf[2];
      if (!readExact(client, crlf, 2, millis() + 8000)) {
        Serial.println("[PCM] missing CRLF after chunk");
        break;
      }
    }
  } else if (contentLen > 0) {
    while (contentLen > 0) {
      size_t take = (size_t)contentLen > sizeof(buf) ? sizeof(buf) : (size_t)contentLen;
      if (!readExact(client, buf, take, pcmDeadline))
        break;
      pcmJbPush(buf, take, &checkedWav);
      totalBytes += take;
      contentLen -= (long)take;
    }
  } else {
    unsigned long lastData = millis();
    while (millis() < pcmDeadline) {
      int avail = client.available();
      if (avail > 0) {
        lastData = millis();
        int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
        if (want & 1) want--;
        int len = client.read(buf, want > 0 ? want : 0);
        if (len > 0) {
          pcmJbPush(buf, (size_t)len, &checkedWav);
          totalBytes += (size_t)len;
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

  pcmJbFlush(&checkedWav);

  client.stop();
  Serial.printf("[AUDIO DONE] %u PCM bytes\n", (unsigned)totalBytes);
  return totalBytes;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[BOOT]");
  connectWiFi();
  setupI2SMic();
  setupI2SSpeaker();
  Serial.println("[READY]");
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
    Serial.println("[VOICE DETECTED]");

    static int16_t fullAudio[16000];
    for (int i = 0; i < 16000; i += BUFFER_SIZE) {
      readMic(fullAudio + i, BUFFER_SIZE);
    }

    String stt = sendSTT(fullAudio, 16000);
    Serial.println("[STT] " + stt);

    String transcript = transcriptFromSttResponse(stt);
    String langHint = languageFromSttResponse(stt);
    transcript.trim();
    Serial.println("[YOU] " + transcript);

    if (transcript.length() > 0) {
      size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
      unsigned long audioMs =
          pcmBytes > 0 ? ((pcmBytes / 2) * 1000UL / (unsigned long)SAMPLE_RATE_OUT) : 0;
      unsigned long guard = audioMs + (unsigned long)VAD_AFTER_PLAY_PADDING_MS;
      if (guard < (unsigned long)VAD_COOLDOWN_MIN_MS)
        guard = (unsigned long)VAD_COOLDOWN_MIN_MS;
      if (guard > 90000UL)
        guard = 90000UL;
      vadCooldownUntilMs = millis() + guard;
      Serial.printf("[VAD] pause %lu ms (~%lu ms audio) — blocks echo re-trigger\n",
                    guard, audioMs);
    } else {
      vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
    }
  }
}
