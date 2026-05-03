#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>

// ================= CONFIG =================
const char* WIFI_SSID = "GALGOTIAS-ARUBA";
const char* WIFI_PASS = "1234567@";
const char* BACKEND = "https://backend.dev1974sai.workers.dev";

// ✅ MATCH YOUR ACTUAL WIRING
#define I2S_WS   15   // LRCLK
#define I2S_SCK  16   // BCLK
#define I2S_SD   4    // DATA

#define SAMPLE_RATE 16000
#define BUFFER_SIZE 1024

int32_t rawBuffer[BUFFER_SIZE];

// ================= I2S =================
void setupI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
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

  Serial.println("[I2S] initialized");
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
    out[i] = rawBuffer[i] >> 14; // 32 → 16 bit
  }
}

// ================= RMS =================
float computeRMS(int16_t* data, int len) {
  float sum = 0;
  for (int i = 0; i < len; i++) {
    sum += data[i] * data[i];
  }
  return sqrt(sum / len);
}

// ================= STT =================
String sendSTT(int16_t* audio, int samples) {
  HTTPClient http;
  http.begin(String(BACKEND) + "/transcribe");

  http.addHeader("Content-Type", "application/octet-stream");

  int size = samples * 2;
  int code = http.POST((uint8_t*)audio, size);

  String res = "";

  if (code > 0) {
    res = http.getString();
  } else {
    Serial.println("[STT] failed");
  }

  http.end();
  return res;
}

// Pull `transcript` and optional `language_code` from /transcribe JSON (no extra library).
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

// JSON-escape into `out` (same idea as voice-to-voice.ino).
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

// ================= CHAT =================
// POST /chat expects JSON { deviceId, text, language_code? }; response is audio/wav (not plain text).
String sendChat(const String& deviceId, const String& text, const String& langHint) {
  HTTPClient http;
  http.begin(String(BACKEND) + "/chat");

  http.addHeader("Content-Type", "application/json");

  String escaped;
  jsonEscapeInto(escaped, text.c_str());

  String body;
  body.reserve(escaped.length() + 96);
  body = "{\"deviceId\":\"";
  body += deviceId;
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

  String res = "";

  if (code > 0) {
    res = http.getString();
  } else {
    Serial.println("[AI] failed");
  }

  http.end();
  return res;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[BOOT] starting...");

  connectWiFi();
  setupI2S();

  Serial.println("[SYSTEM] ready");
}

// ================= LOOP =================
void loop() {
  static int16_t audio[BUFFER_SIZE];

  readMic(audio, BUFFER_SIZE);

  float rms = computeRMS(audio, BUFFER_SIZE);

  int peak = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    peak = max(peak, abs(audio[i]));
  }

  Serial.print("[rms] ");
  Serial.print((int)rms);
  Serial.print(" | peak ");
  Serial.println(peak);

  // ===== SIMPLE VAD =====
  if (peak > 2000) {
    Serial.println("[voice] detected");

    static int16_t fullAudio[16000];

    for (int i = 0; i < 16000; i += BUFFER_SIZE) {
      readMic(fullAudio + i, BUFFER_SIZE);
    }

    Serial.println("[net] sending STT...");
    String text = sendSTT(fullAudio, 16000);

    Serial.println("[STT] " + text);

    String transcript = transcriptFromSttResponse(text);
    String langHint = languageFromSttResponse(text);

    if (transcript.length() == 0) {
      Serial.println("[chat] skipped (empty transcript)");
    } else {
      Serial.println("[net] sending to AI...");
      String reply =
          sendChat(deviceIdFromMac(), transcript, langHint);

      Serial.println("[AI] " + reply);
    }

    delay(2000);
  }
}