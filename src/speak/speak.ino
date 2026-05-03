#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <driver/i2s.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ================= CONFIG =================
const char* WIFI_SSID = "GALGOTIAS-ARUBA";
const char* WIFI_PASS = "1234567@";
static const char* BACKEND_HOST = "backend.dev1974sai.workers.dev";

/** NVS namespace for saved STA credentials (portal writes ssid + pass). */
#ifndef OPEN_EVE_PREFS_NS
#define OPEN_EVE_PREFS_NS "openeve"
#endif
#ifndef OPEN_EVE_WIFI_STA_TIMEOUT_MS
#define OPEN_EVE_WIFI_STA_TIMEOUT_MS 45000UL
#endif
/** SoftAP name prefix; last two MAC hex digits are appended (e.g. OpenEve-A1B2). */
#ifndef OPEN_EVE_AP_SSID_PREFIX
#define OPEN_EVE_AP_SSID_PREFIX "OpenEveBySaidevDhal-"
#endif
/** WPA2 passphrase for the setup hotspot (min 8 chars). Change before shipping. */
#ifndef OPEN_EVE_AP_PASS
#define OPEN_EVE_AP_PASS "Gu@OpenEve"
#endif

/** Set to 0 to restore always-listening (every utterance goes to /chat). */
#ifndef OPEN_EVE_REQUIRE_WAKE_WORD
#define OPEN_EVE_REQUIRE_WAKE_WORD 1
#endif
#ifndef OPEN_EVE_WAKE_WORD
#define OPEN_EVE_WAKE_WORD "eve"
#endif
/** Devanagari “ईव” (long ई + व) — UTF-8 must use 0xE0 0xA4 (not 0xE0 0xA5) for letters U+0900–U+097F. */
#ifndef OPEN_EVE_WAKE_WORD_HI_UTF8
#define OPEN_EVE_WAKE_WORD_HI_UTF8 "\xe0\xa4\x88\xe0\xa4\xb5"
#endif
/** Alternate Hindi “इव” (short इ + व) — usual STT output for spoken “Eve”. */
#ifndef OPEN_EVE_WAKE_WORD_HI_ALT_UTF8
#define OPEN_EVE_WAKE_WORD_HI_ALT_UTF8 "\xe0\xa4\x87\xe0\xa4\xb5"
#endif
/** Odia ଇଭ୍ — Sarvam / common STT spelling for “Eve” (I + consonant cluster + halant). */
#ifndef OPEN_EVE_WAKE_WORD_OD_UTF8
#define OPEN_EVE_WAKE_WORD_OD_UTF8 "\xe0\xac\x87\xe0\xac\xad\xe0\xad\x8d"
#endif
/** Odia ଇଭ — variant if STT omits trailing halant before punctuation. */
#ifndef OPEN_EVE_WAKE_WORD_OD_ALT_UTF8
#define OPEN_EVE_WAKE_WORD_OD_ALT_UTF8 "\xe0\xac\x87\xe0\xac\xad"
#endif
/** Bengali ইভ */
#ifndef OPEN_EVE_WAKE_WORD_BN_UTF8
#define OPEN_EVE_WAKE_WORD_BN_UTF8 "\xe0\xa6\x87\xe0\xa6\xad"
#endif
/** Telugu ఇవ */
#ifndef OPEN_EVE_WAKE_WORD_TE_UTF8
#define OPEN_EVE_WAKE_WORD_TE_UTF8 "\xe0\xb0\x87\xe0\xb0\xb5"
#endif
/** Kannada ಇವ */
#ifndef OPEN_EVE_WAKE_WORD_KN_UTF8
#define OPEN_EVE_WAKE_WORD_KN_UTF8 "\xe0\xb2\x87\xe0\xb2\xb5"
#endif
/** Malayalam ഇവ */
#ifndef OPEN_EVE_WAKE_WORD_ML_UTF8
#define OPEN_EVE_WAKE_WORD_ML_UTF8 "\xe0\xb4\x87\xe0\xb4\xb5"
#endif
/** Tamil இவெ */
#ifndef OPEN_EVE_WAKE_WORD_TA_UTF8
#define OPEN_EVE_WAKE_WORD_TA_UTF8 "\xe0\xae\x87\xe0\xae\xb5\xe0\xaf\x87"
#endif
/** Gujarati ઇવ */
#ifndef OPEN_EVE_WAKE_WORD_GU_UTF8
#define OPEN_EVE_WAKE_WORD_GU_UTF8 "\xe0\xaa\x87\xe0\xaa\xb5"
#endif
/** Gurmukhi ਇਵ */
#ifndef OPEN_EVE_WAKE_WORD_PA_UTF8
#define OPEN_EVE_WAKE_WORD_PA_UTF8 "\xe0\xa8\x87\xe0\xa8\xb5"
#endif

/**
 * Offline wake (no HTTP while idle): active-low GPIO (button to GND). On a press edge,
 * ONE following utterance may use STT + /chat + TTS. While not armed, loud speech is ignored
 * locally (no upload, no cloud STT, no Worker).
 *
 * Spoken “eve” without cloud STT is not implementable in plain C++ on the ESP32 — you need
 * on-device WakeNet / ESP_SR (ESP32-S3 + SR partition + trained model) or a third-party wake lib.
 * Set this to **-1** to use transcript-based wake instead (calls /transcribe on every utterance).
 */
#ifndef OPEN_EVE_HARDWARE_WAKE_GPIO
#define OPEN_EVE_HARDWARE_WAKE_GPIO -1
#endif

#if OPEN_EVE_REQUIRE_WAKE_WORD && OPEN_EVE_HARDWARE_WAKE_GPIO < 0
#warning Open EvE: transcript wake uses Worker /transcribe (Sarvam STT) on every loud utterance. Set OPEN_EVE_HARDWARE_WAKE_GPIO>=0 for no cloud STT until wake button, or say wake+command in one sentence (still one STT).
#endif

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
/** Upper bound @ 16 kHz (~10 s). Long “wake + web search …” prompts need ≥8–15 s spoken;
 * raise via build flag if you have heap (~320 KiB PCM here); Worker + Sarvam allow ~30 s via STT limits. */
#define STT_MAX_CAPTURE_SAMPLES 160000
#endif
#ifndef STT_MIN_CAPTURE_SAMPLES
/** Require at least this much audio before silence can end capture (avoid cutting first word). */
#define STT_MIN_CAPTURE_SAMPLES 14000
#endif
#ifndef STT_END_SILENCE_MS
/** Stop recording after this many ms below STT_END_SILENCE_PEAK (end of utterance).
 * Long commands often have brief pauses — too low cuts off before the user finishes. */
#define STT_END_SILENCE_MS 1300
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
#define STT_HTTP_TIMEOUT_MS 90000
#endif

#ifndef PCM_STREAM_STALL_MS
#define PCM_STREAM_STALL_MS 8000
#endif

#ifndef VAD_PEAK_THRESHOLD
#define VAD_PEAK_THRESHOLD 1900
#endif

/** Max PCM body for /chat buffered mode (CHAT_PCM_PLAY_MODE 0): entire reply held in heap before play.
 * 448 KiB ≈ 9.4 s @ 24 kHz mono — long list/news TTS hits this and truncates (“buffer limit”).
 * Stream mode (CHAT_PCM_PLAY_MODE 1) plays while downloading — uses PCM_JITTER_BYTES instead for peak RAM.
 * Lower if TLS/chat fails OOM; raise if you have RAM (PSRAM) and need longer one-shot replies. */
#ifndef CHAT_PCM_MAX_BYTES
#define CHAT_PCM_MAX_BYTES (896 * 1024)
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
/** Peak-normalize before boost / gain (~85% scale headroom); soft limiting handles hotter peaks downstream. */
#define PCM_NORMALIZE_PEAK_TARGET 28000
#endif
#ifndef PCM_SOFT_LIMIT_THRESHOLD
/** Above +/- this magnitude, squash toward +-32767 (ratio curve) instead of hard clip. */
#define PCM_SOFT_LIMIT_THRESHOLD 30000
#endif
#ifndef PCM_NORMALIZE_NOISE_FLOOR
/** Below this abs peak in clip, skip normalization (quiet TTS still scales if above floor). */
#define PCM_NORMALIZE_NOISE_FLOOR 22
#endif

/** Extra digital push after normalize (buffered mode): lifts RMS; peaks clip — louder but hotter. */
#ifndef PCM_SATURATE_BOOST_PERCENT
#define PCM_SATURATE_BOOST_PERCENT 120
#endif

/** I2S write gain: buffered path uses lower value because saturate boost already ran. */
#ifndef BUFFERED_PLAYBACK_GAIN_PERCENT
#define BUFFERED_PLAYBACK_GAIN_PERCENT 140
#endif
#ifndef STREAM_PLAYBACK_GAIN_PERCENT
#define STREAM_PLAYBACK_GAIN_PERCENT 220
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

static WebServer* gProvServer = nullptr;
static DNSServer gDnsServer;

static void loadWifiCredentials(String& ssid, String& pass) {
  Preferences prefs;
  if (prefs.begin(OPEN_EVE_PREFS_NS, true)) {
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
  } else {
    ssid = "";
    pass = "";
  }
  if (ssid.length() == 0) {
    ssid = WIFI_SSID;
    pass = WIFI_PASS;
  }
}

static bool connectWiFiSta(const String& ssid, const String& pass) {
  if (ssid.length() == 0) {
    Serial.println("[WiFi] no SSID (use provisioning portal)");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);

  if (pass.length() > 0)
    WiFi.begin(ssid.c_str(), pass.c_str());
  else
    WiFi.begin(ssid.c_str());

  Serial.printf("[WiFi] STA connecting to \"%s\"…\n", ssid.c_str());
  Serial.print("[WiFi]");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - t0 < OPEN_EVE_WIFI_STA_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
    yield();
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] STA failed or timed out");
    WiFi.disconnect(true);
    delay(100);
    return false;
  }

  WiFi.setSleep(false);
  Serial.print("[WiFi] connected, IP ");
  Serial.println(WiFi.localIP());
  return true;
}

static void handleProvRoot() {
  if (!gProvServer)
    return;
  static const char html[] =
      "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
      "content=\"width=device-width\"><title>Open EvE WiFi</title></head><body>"
      "<h1>WiFi setup</h1>"
      "<p>Join this device’s hotspot, then enter your 2.4&nbsp;GHz network.</p>"
      "<form method=\"POST\" action=\"/save\">"
      "<p><label>SSID<br><input name=\"ssid\" maxlength=\"32\" required "
      "autocomplete=\"off\"></label></p>"
      "<p><label>Password<br><input name=\"pass\" type=\"password\" maxlength=\"64\" "
      "autocomplete=\"off\"></label></p>"
      "<p><button type=\"submit\">Save &amp; reboot</button></p>"
      "</form></body></html>";
  gProvServer->send(200, "text/html", html);
}

static void handleProvSave() {
  if (!gProvServer)
    return;
  if (!gProvServer->hasArg("ssid")) {
    gProvServer->send(400, "text/plain", "missing ssid");
    return;
  }

  String ssid = gProvServer->arg("ssid");
  String pw = gProvServer->hasArg("pass") ? gProvServer->arg("pass") : "";
  ssid.trim();
  pw.trim();

  if (ssid.length() == 0 || ssid.length() > 32) {
    gProvServer->send(400, "text/plain", "invalid ssid");
    return;
  }
  if (pw.length() > 64) {
    gProvServer->send(400, "text/plain", "invalid password length");
    return;
  }

  Preferences prefs;
  if (!prefs.begin(OPEN_EVE_PREFS_NS, false)) {
    gProvServer->send(500, "text/plain", "storage error");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pw);
  prefs.end();

  Serial.printf("[WiFi] saved SSID \"%s\" — rebooting\n", ssid.c_str());
  gProvServer->send(200, "text/html",
                     "<!DOCTYPE html><html><body><p>Saved. Rebooting…</p></body></html>");
  delay(400);
  ESP.restart();
}

/** Blocks until credentials submitted or device reset; never returns except via ESP.restart(). */
static void runWifiProvisioningPortal() {
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_AP);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apName[33];
  snprintf(apName, sizeof(apName), "%s%02X%02X", OPEN_EVE_AP_SSID_PREFIX, mac[4], mac[5]);

  if (!WiFi.softAP(apName, OPEN_EVE_AP_PASS)) {
    Serial.println("[WiFi] softAP failed — fix OPEN_EVE_AP_SSID_PREFIX / channel");
    while (true) {
      delay(1000);
      yield();
    }
  }

  IPAddress apIp = WiFi.softAPIP();
  Serial.printf("[WiFi] SoftAP \"%s\" — use WPA2 password from OPEN_EVE_AP_PASS in sketch\n",
                apName);
  Serial.printf("[WiFi] Captive portal / setup: http://%s/\n", apIp.toString().c_str());

  gDnsServer.stop();
  gDnsServer.start(53, "*", apIp);

  static WebServer server(80);
  gProvServer = &server;

  server.on("/", HTTP_GET, handleProvRoot);
  server.on("/save", HTTP_POST, handleProvSave);
  server.onNotFound([]() {
    if (!gProvServer)
      return;
    gProvServer->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/",
                             true);
    gProvServer->send(302, "text/plain", "");
  });

  server.begin();

  while (true) {
    gDnsServer.processNextRequest();
    server.handleClient();
    delay(2);
    yield();
  }
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

// ================= STT: one /transcribe per VAD utterance (Worker → Sarvam) =================
// PCM → text always needs speech recognition. This firmware does not run a second STT for “wake”
// vs “command” — there is a single POST with the whole clip. Wake checks are string rules on that
// transcript (no second STT for “wake” vs “command”). To avoid paying STT on random room noise, use
// OPEN_EVE_HARDWARE_WAKE_GPIO (no upload until armed) or on-device wake (ESP-SR / WakeNet).
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

#if OPEN_EVE_REQUIRE_WAKE_WORD && OPEN_EVE_HARDWARE_WAKE_GPIO < 0
static bool gWakeCommandArmed = false;

static inline bool isAsciiLatinLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/** Trim ASCII punctuation / whitespace from both ends without touching UTF-8 Devanagari bytes. */
static void stripAsciiEdges(String& s) {
  const char* punct = ".,!?;: \t\r\n";
  bool changed = true;
  while (changed && s.length()) {
    changed = false;
    char c0 = s.charAt(0);
    if ((uint8_t)(unsigned char)c0 < 128U && strchr(punct, c0)) {
      s.remove(0, 1);
      changed = true;
      continue;
    }
    char c1 = s.charAt(s.length() - 1);
    if ((uint8_t)(unsigned char)c1 < 128U && strchr(punct, c1)) {
      s.remove(s.length() - 1);
      changed = true;
    }
  }
}

/** Lowercase ASCII string; wake is lowercase — boundaries so "Hello Eve," matches but not "evening". */
static bool latinWakeWordPresent(const String& lower, const String& wake) {
  int wl = wake.length();
  if (wl <= 0 || (int)lower.length() < wl)
    return false;
  int searchStart = 0;
  for (;;) {
    int idx = lower.indexOf(wake, searchStart);
    if (idx < 0)
      return false;
    bool okBefore =
        idx == 0 || !isAsciiLatinLetter(lower.charAt(idx - 1));
    int afterIdx = idx + wl;
    bool okAfter =
        afterIdx >= (int)lower.length() ||
        !isAsciiLatinLetter(lower.charAt(afterIdx));
    if (okBefore && okAfter)
      return true;
    searchStart = idx + 1;
  }
}

static bool transcriptContainsWakeWord(const String& transcript) {
  String t = transcript;
  t.trim();
  if (t.length() == 0)
    return false;

  static const char* const kUtf8Wakes[] = {
    OPEN_EVE_WAKE_WORD_HI_UTF8,
    OPEN_EVE_WAKE_WORD_HI_ALT_UTF8,
    OPEN_EVE_WAKE_WORD_OD_UTF8,
    OPEN_EVE_WAKE_WORD_OD_ALT_UTF8,
    OPEN_EVE_WAKE_WORD_BN_UTF8,
    OPEN_EVE_WAKE_WORD_TE_UTF8,
    OPEN_EVE_WAKE_WORD_KN_UTF8,
    OPEN_EVE_WAKE_WORD_ML_UTF8,
    OPEN_EVE_WAKE_WORD_TA_UTF8,
    OPEN_EVE_WAKE_WORD_GU_UTF8,
    OPEN_EVE_WAKE_WORD_PA_UTF8,
    nullptr,
  };

  for (int i = 0; kUtf8Wakes[i] != nullptr; i++) {
    if (strlen(kUtf8Wakes[i]) && t.indexOf(String(kUtf8Wakes[i])) >= 0)
      return true;
  }

  String lo = t;
  lo.toLowerCase();
  String wake = String(OPEN_EVE_WAKE_WORD);
  if (wake.length() == 0)
    return false;

  return latinWakeWordPresent(lo, wake);
}

/** True only when the utterance is just the wake token (Latin or Indic spellings below), optional ASCII punctuation. */
static bool utteranceIsWakeOnly(const String& transcript) {
  String s = transcript;
  s.trim();
  if (!s.length())
    return false;

  String latin = s;
  stripAsciiEdges(latin);
  latin.toLowerCase();
  if (latin.length() && latin == String(OPEN_EVE_WAKE_WORD))
    return true;

  String hi = s;
  stripAsciiEdges(hi);

  static const char* const kWakeOnlyUtf8[] = {
    OPEN_EVE_WAKE_WORD_HI_UTF8,
    OPEN_EVE_WAKE_WORD_HI_ALT_UTF8,
    OPEN_EVE_WAKE_WORD_OD_UTF8,
    OPEN_EVE_WAKE_WORD_OD_ALT_UTF8,
    OPEN_EVE_WAKE_WORD_BN_UTF8,
    OPEN_EVE_WAKE_WORD_TE_UTF8,
    OPEN_EVE_WAKE_WORD_KN_UTF8,
    OPEN_EVE_WAKE_WORD_ML_UTF8,
    OPEN_EVE_WAKE_WORD_TA_UTF8,
    OPEN_EVE_WAKE_WORD_GU_UTF8,
    OPEN_EVE_WAKE_WORD_PA_UTF8,
    nullptr,
  };
  if (hi.length()) {
    for (int i = 0; kWakeOnlyUtf8[i] != nullptr; i++) {
      if (!strlen(kWakeOnlyUtf8[i])) continue;
      if (hi == String(kWakeOnlyUtf8[i])) return true;
    }
  }

  return false;
}
#endif

#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
static bool gHwWakePendingUtterance = false;
static bool gHwWakePrevPinLow = false;
#endif

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

/** Symmetric soft clip: |x| > threshold → smooth toward ±32767 (no hard edge at threshold). */
static int16_t pcmSoftLimitInt64ToS16(int64_t x) {
  const int32_t T = PCM_SOFT_LIMIT_THRESHOLD;
  const int32_t R = 32767 - T;
  if (x > T) {
    int64_t over = x - T;
    if (over < 0) over = 0;
    int64_t y = T + (over * R) / (R + over);
    if (y > 32767) y = 32767;
    return (int16_t)y;
  }
  if (x < -T) {
    int64_t over = -T - x;
    if (over < 0) over = 0;
    int64_t y = -T - (over * R) / (R + over);
    if (y < -32768) y = -32768;
    return (int16_t)y;
  }
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

/**
 * Peak-normalize to PCM_NORMALIZE_PEAK_TARGET (~28000), then soft-limit instead of hard clamp.
 * Call once on the full clip before playback.
 */
static void normalizePcmS16SoftLimited(int16_t* s, size_t nSamp) {
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
    s[i] = pcmSoftLimitInt64ToS16(x);
  }
  Serial.printf("[PCM] soft-normalize abs_peak=%ld → target=%d (limit @ ±%d)\n",
                (long)peak, PCM_NORMALIZE_PEAK_TARGET, PCM_SOFT_LIMIT_THRESHOLD);
}

static void pcmSaturateBoostInPlace(int16_t* s, size_t nSamp, unsigned pct) {
  if (nSamp == 0 || pct == 100)
    return;
  for (size_t i = 0; i < nSamp; i++) {
    int64_t x = ((int64_t)s[i] * (int64_t)pct) / 100;
    s[i] = pcmSoftLimitInt64ToS16(x);
  }
}

/** Drain PCM to I2S @ SAMPLE_RATE_OUT (24 kHz, match Worker TTS). Gain in int64, soft-limit, assign to slots. */
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
      int64_t g = ((int64_t)ps[i] * (int64_t)g_pcmWriteGainPct) / 100;
      int16_t out = pcmSoftLimitInt64ToS16(g);
      txSlots[i] = ((int32_t)out) << 16;
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
    normalizePcmS16SoftLimited((int16_t*)playPtr, playLen / 2);
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
          Serial.println("[PCM] buffer limit — draining chunk then stopping capture "
                         "(raise CHAT_PCM_MAX_BYTES or CHAT_PCM_PLAY_MODE 1)");
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
        Serial.println("[PCM] buffer limit (raise CHAT_PCM_MAX_BYTES or CHAT_PCM_PLAY_MODE 1)");
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
            Serial.println("[PCM] buffer limit (raise CHAT_PCM_MAX_BYTES or CHAT_PCM_PLAY_MODE 1)");
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

static unsigned long pcmBytesToAudioMs(size_t pcmBytes) {
  pcmBytes &= ~(size_t)1;
  return pcmBytes > 0 ? ((pcmBytes / 2) * 1000UL / (unsigned long)SAMPLE_RATE_OUT) : 0UL;
}

static unsigned long computeMuteGuardMs(unsigned long audioMs) {
  unsigned long extraLong =
      audioMs / 8UL > (unsigned long)VAD_LONG_PLAY_EXTRA_CAP_MS
          ? (unsigned long)VAD_LONG_PLAY_EXTRA_CAP_MS
          : audioMs / 8UL;
  unsigned long guard = (unsigned long)VAD_AFTER_PLAY_PADDING_MS + extraLong;
  if (guard < (unsigned long)VAD_COOLDOWN_MIN_MS)
    guard = (unsigned long)VAD_COOLDOWN_MIN_MS;
  if (guard > 90000UL)
    guard = 90000UL;
  return guard;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[BOOT]");
  Serial.printf("[BOOT] heap=%u\n", (unsigned)ESP.getFreeHeap());

  String wifiSsid;
  String wifiPass;
  loadWifiCredentials(wifiSsid, wifiPass);
  if (!connectWiFiSta(wifiSsid, wifiPass)) {
    Serial.println("[WiFi] starting provisioning portal (STA unavailable)");
    runWifiProvisioningPortal();
  }

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
#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
  pinMode(OPEN_EVE_HARDWARE_WAKE_GPIO, INPUT_PULLUP);
  Serial.printf("[CONFIG] Hardware wake GPIO %d (active LOW) — idle audio stays offline\n",
                OPEN_EVE_HARDWARE_WAKE_GPIO);
#elif OPEN_EVE_REQUIRE_WAKE_WORD
  Serial.print("[CONFIG] Cloud wake (STT every utterance), token: ");
  Serial.println(OPEN_EVE_WAKE_WORD);
#endif
  Serial.printf("[READY] heap=%u\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  static int16_t audio[BUFFER_SIZE];
  static unsigned long vadCooldownUntilMs = 0;

#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
  bool pinLow = digitalRead(OPEN_EVE_HARDWARE_WAKE_GPIO) == LOW;
  if (!gHwWakePendingUtterance && pinLow && !gHwWakePrevPinLow) {
    gHwWakePendingUtterance = true;
    Serial.println("[WAKE] armed (GPIO) — speak now");
    vadCooldownUntilMs = millis() + 400;
  }
  gHwWakePrevPinLow = pinLow;
#endif

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
#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
    if (!gHwWakePendingUtterance) {
      Serial.println("[WAKE] idle (offline) — press wake GPIO then speak");
      vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
      return;
    }
#endif
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

    if (totalWritten >= (size_t)STT_MAX_CAPTURE_SAMPLES &&
        silentRunSamples < silentNeedSamples) {
      Serial.println("[STT] WARN: capture hit STT_MAX_CAPTURE_SAMPLES before silence — phrase may be "
                     "truncated; increase STT_MAX_CAPTURE_SAMPLES if you have heap");
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

    if (transcript.length() == 0) {
#if OPEN_EVE_REQUIRE_WAKE_WORD && OPEN_EVE_HARDWARE_WAKE_GPIO < 0
      if (gWakeCommandArmed) {
        gWakeCommandArmed = false;
        Serial.println("[WAKE] disarmed (empty transcript)");
      }
#endif
      vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
    } else {
#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
      size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
      unsigned long audioMs = pcmBytesToAudioMs(pcmBytes);
      unsigned long guardMs = computeMuteGuardMs(audioMs);
      vadCooldownUntilMs = millis() + guardMs;
      Serial.printf("[VAD] mute-after-play %lu ms (spoken ~%lu ms; tail blocks echo)\n",
                    guardMs, audioMs);
#elif OPEN_EVE_REQUIRE_WAKE_WORD
      if (!gWakeCommandArmed) {
        if (!transcriptContainsWakeWord(transcript)) {
          Serial.println("[WAKE] ignored — say \"eve\" (English) or your language’s spelling of Eve (Odia ଇଭ୍, Hindi इव…)");
          vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
        } else if (utteranceIsWakeOnly(transcript)) {
          gWakeCommandArmed = true;
          Serial.println("[WAKE] listening — ask your question");
          vadCooldownUntilMs = millis() + (unsigned long)VAD_COOLDOWN_EMPTY_MS;
        } else {
          gWakeCommandArmed = false;
          size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
          unsigned long audioMs = pcmBytesToAudioMs(pcmBytes);
          unsigned long guardMs = computeMuteGuardMs(audioMs);
          vadCooldownUntilMs = millis() + guardMs;
          Serial.printf("[VAD] mute-after-play %lu ms (spoken ~%lu ms; tail blocks echo)\n",
                        guardMs, audioMs);
        }
      } else {
        gWakeCommandArmed = false;
        size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
        unsigned long audioMs = pcmBytesToAudioMs(pcmBytes);
        unsigned long guardMs = computeMuteGuardMs(audioMs);
        vadCooldownUntilMs = millis() + guardMs;
        Serial.printf("[VAD] mute-after-play %lu ms (spoken ~%lu ms; tail blocks echo)\n",
                      guardMs, audioMs);
      }
#else
      size_t pcmBytes = sendChatAndSpeak(transcript, langHint);
      unsigned long audioMs = pcmBytesToAudioMs(pcmBytes);
      unsigned long guardMs = computeMuteGuardMs(audioMs);
      vadCooldownUntilMs = millis() + guardMs;
      Serial.printf("[VAD] mute-after-play %lu ms (spoken ~%lu ms; tail blocks echo)\n",
                    guardMs, audioMs);
#endif
    }

#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
    gHwWakePendingUtterance = false;
#endif
  }
}
