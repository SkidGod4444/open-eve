#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>

// ================= CONFIG =================
const char* WIFI_SSID = "GALGOTIAS-ARUBA";
const char* WIFI_PASS = "1234567@";
const char* BACKEND = "https://backend.dev1974sai.workers.dev";

// ===== MIC (I2S0) =====
#define I2S_WS    15
#define I2S_SCK   16
#define I2S_SD    4

// ===== SPEAKER (I2S1) =====
#define I2S_OUT_WS   17
#define I2S_OUT_SCK  18
#define I2S_OUT_SD   21

#define SAMPLE_RATE_MIC 16000
#define SAMPLE_RATE_OUT 24000   // Must match Worker / Sarvam TTS (linear16 @ 24 kHz mono)

#define BUFFER_SIZE 1024

#ifndef CHAT_HTTP_TIMEOUT_MS
#define CHAT_HTTP_TIMEOUT_MS 180000
#endif
#ifndef STT_HTTP_TIMEOUT_MS
#define STT_HTTP_TIMEOUT_MS 60000
#endif

// Stall detection between chunks (chunked PCM); tune if flaky Wi‑Fi.
#ifndef PCM_STREAM_STALL_MS
#define PCM_STREAM_STALL_MS 8000
#endif

int32_t rawBuffer[BUFFER_SIZE];

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
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256
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

  Serial.println("[I2S SPEAKER] ready (24 kHz mono PCM)");
}

// ================= WIFI =================
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[WiFi] connected");
}

// ================= MIC READ =================
void readMic(int16_t* out, int samples) {
  size_t bytesRead;
  i2s_read(I2S_NUM_0, rawBuffer, samples * 4, &bytesRead, portMAX_DELAY);

  for (int i = 0; i < samples; i++) {
    out[i] = rawBuffer[i] >> 14;
  }
}

// ================= STT =================
String sendSTT(int16_t* audio, int samples) {
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(STT_HTTP_TIMEOUT_MS);
  http.begin(String(BACKEND) + "/transcribe");

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

/**
 * Backend returns raw s16le mono PCM @ 24 kHz — NO WAV header.
 * Do not skip 44 bytes; that would destroy alignment and cause ticking/noise.
 */
void playPcmStreamFromHttp(HTTPClient& http) {
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("[PCM] no stream");
    return;
  }

  uint8_t buf[1024];
  bool checkedHeader = false;
  bool seenPcm = false;
  unsigned long lastDataMs = 0;
  unsigned long wallEnd = millis() + CHAT_HTTP_TIMEOUT_MS;

  while (millis() < wallEnd) {
    int avail = stream->available();

    if (avail > 0) {
      seenPcm = true;
      lastDataMs = millis();

      int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
      if (want & 1) want--;

      int len = stream->readBytes(buf, want > 0 ? want : 0);
      if (len <= 0)
        continue;

      if (!checkedHeader && len >= 4) {
        checkedHeader = true;
        if (memcmp(buf, "RIFF", 4) == 0) {
          Serial.println("[PCM] WARNING: response looks like WAV — backend should send raw PCM only");
        }
      }

      if (len & 1) len--;

      size_t written = 0;
      i2s_write(I2S_NUM_1, buf, (size_t)len, &written, portMAX_DELAY);
      continue;
    }

    if (!http.connected())
      break;

    // Only treat idle as stall after audio has started (first byte can wait for slow LLM+TTS).
    if (seenPcm && lastDataMs > 0 && (millis() - lastDataMs > PCM_STREAM_STALL_MS)) {
      Serial.println("[PCM] stalled — ending playback");
      break;
    }

    delay(1);
  }

  Serial.println("[AUDIO DONE]");
}

void sendChatAndSpeak(const String& transcript, const String& langHint) {
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(CHAT_HTTP_TIMEOUT_MS);
  http.begin(String(BACKEND) + "/chat");

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/octet-stream");

  String escaped;
  jsonEscapeInto(escaped, transcript.c_str());

  String body;
  body.reserve(escaped.length() + 128);
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

  int code = http.POST(body);

  if (code == HTTP_CODE_OK) {
    Serial.printf("[AI] speaking… (%s)\n", http.header("Content-Type").c_str());
    playPcmStreamFromHttp(http);
  } else if (code > 0) {
    Serial.printf("[AI] HTTP %d: ", code);
    Serial.println(http.getString());
  } else {
    Serial.printf("[AI] transport error %d (%s)\n",
                  code, HTTPClient::errorToString(code).c_str());
  }

  http.end();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("[BOOT]");

  connectWiFi();
  setupI2SMic();
  setupI2SSpeaker();

  Serial.println("[READY]");
}

// ================= LOOP =================
void loop() {
  static int16_t audio[BUFFER_SIZE];

  readMic(audio, BUFFER_SIZE);

  int peak = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    peak = max(peak, abs(audio[i]));
  }

  if (peak > 2000) {
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
      sendChatAndSpeak(transcript, langHint);
    }

    delay(2000);
  }
}
