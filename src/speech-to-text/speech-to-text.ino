// Open EvE - ESP32 speech-to-text client with on-device VAD.
//
// Continuously listens via an INMP441 I2S mic, runs an adaptive
// energy-based Voice Activity Detector, and only POSTs an utterance to the
// Open EvE backend (Cloudflare Worker -> Sarvam Saaras v3) when the
// captured audio actually looks like speech. Background noise, taps,
// clicks, and steady fans/AC are filtered out before any HTTP call.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Pins / Wi-Fi / server
// ---------------------------------------------------------------------------

#define I2S_WS  25
#define I2S_SD  33
#define I2S_SCK 26

const char* ssid     = "GALGOTIAS-ARUBA";
const char* password = "1234567@";

// Backend endpoint. Query params tell the Worker how to interpret the raw
// PCM body and which language hint to forward to Sarvam.
const char* server =
  "https://backend.dev1974sai.workers.dev/transcribe"
  "?sample_rate=16000"
  "&channels=1"
  "&bits_per_sample=16"
  "&language_code=hi-IN"
  "&mode=transcribe";

// ---------------------------------------------------------------------------
// Audio + VAD configuration  (TUNE THESE)
// ---------------------------------------------------------------------------

static const int      SAMPLE_RATE       = 16000;

// One short analysis frame ~ 20 ms. The whole VAD operates frame-by-frame.
static const int      FRAME_MS          = 20;
static const size_t   FRAME_SAMPLES     = (SAMPLE_RATE * FRAME_MS) / 1000;       // 320

// Maximum length of a single uploaded utterance. Sarvam's sync limit is 30 s,
// but each second costs ~32 KB of buffer RAM. The runtime allocator below
// will automatically fall back to a shorter length if the requested size
// does not fit (stock ESP32 DRAM is fragmented; total free heap can be
// 250+ KB while the largest single block is < 100 KB).
//   3 s = 96 KB   default, safe on stock ESP32 (no PSRAM)
//   4 s = 128 KB  often fails on stock ESP32, fits comfortably with PSRAM
//   8 s = 256 KB  PSRAM-only
//   30 s = 960 KB PSRAM-only, near Sarvam's hard limit
static const int      MAX_UTTERANCE_SECONDS = 3;

// Pre-roll: how much audio before the trigger frame we splice into the
// recording, so the first phoneme of the first word survives.
static const int      PREROLL_FRAMES        = 15;                                // 300 ms

// Hangover: how long we keep recording after the level drops below the
// release threshold, so a brief pause inside a word doesn't end the utterance.
static const int      HANGOVER_FRAMES       = 35;                                // 700 ms

// Reject any "utterance" with fewer than this many frames of actual voice
// activity -- almost always a tap, click, cough, or door slam.
static const int      MIN_VOICED_FRAMES     = 18;                                // 360 ms

// INMP441 24-bit sample lives in the upper 24 bits of the 32-bit I2S slot.
// MIC_SHIFT is the right-shift used to map it into int16. Lower = louder.
//   16 -> unity gain, very quiet
//   14 -> 4x  gain
//   11 -> 32x gain  (good for normal close-talk speech, default)
//    8 -> 256x gain (clips on anything but a whisper)
static const int      MIC_SHIFT             = 11;

// ----- VAD thresholds ------------------------------------------------------

// Trigger requires BOTH: (a) frame RMS > TRIGGER_SNR x adaptive noise floor,
// and (b) frame peak > ABSOLUTE_MIN_PEAK. (a) rejects steady noise; (b) sets
// the effective max capture distance regardless of how quiet the room is.
//
// Conceptually:
//   TRIGGER_SNR = 3.0  -> need ~10 dB above background to start recording
//   RELEASE_SNR = 1.8  -> drop only when ~5 dB above background (hysteresis)
static const float    TRIGGER_SNR           = 3.0f;
static const float    RELEASE_SNR           = 1.8f;

// Absolute peak floor. Speech at ~30 cm from the mic with MIC_SHIFT=11 peaks
// around 4000-12000. A whisper from 2 m peaks ~500-800 -- which we want to
// IGNORE. Raise this to require louder/closer speech.
static const int16_t  ABSOLUTE_MIN_PEAK     = 1200;

// Adaptive noise-floor smoothing. Higher = adapts faster, lower = smoother.
static const float    NOISE_EMA_ALPHA       = 0.05f;
static const float    NOISE_FLOOR_INIT      = 200.0f;
// Clamp the noise-floor estimate so a long burst of silence (or a long
// burst of sound) can't drive it to absurd values.
static const float    NOISE_FLOOR_MIN       = 80.0f;
static const float    NOISE_FLOOR_MAX       = 4000.0f;

// How often (in frames) to print the idle noise floor for tuning.
static const int      NOISE_LOG_PERIOD      = 100;                               // ~2 s

// ---------------------------------------------------------------------------
// Types
//
// IMPORTANT: define these BEFORE the first function in this .ino file.
// The Arduino IDE auto-inserts forward declarations for every function near
// the top of the sketch, and any struct/enum used in a function signature
// must already be declared at that point or compilation fails with
// "'FrameStats' has not been declared".
// ---------------------------------------------------------------------------

struct FrameStats {
  uint16_t peak;   // max abs sample, 0..32767
  float    rms;    // root-mean-square amplitude
};

enum VadState { VAD_IDLE, VAD_RECORDING };

// ---------------------------------------------------------------------------
// Buffers / state (heap-allocated to keep .bss small)
// ---------------------------------------------------------------------------

int16_t* pcm_buffer       = nullptr;       // utterance under construction
size_t   pcm_capacity_samples = 0;         // actual capacity after fallback
size_t   pcm_capacity_bytes   = 0;
int      pcm_capacity_seconds = 0;

int16_t* preroll_buf = nullptr;            // circular preroll
size_t   preroll_pos = 0;                  // next write index in samples
static const size_t PREROLL_TOTAL_SAMPLES = (size_t)PREROLL_FRAMES * FRAME_SAMPLES;

WiFiClientSecure tls;

VadState vad_state           = VAD_IDLE;
size_t   utt_samples         = 0;          // samples written into pcm_buffer
int      utt_voiced_frames   = 0;          // frames above release threshold
int      utt_hangover_frames = 0;          // consecutive sub-release frames
float    noise_floor_rms     = NOISE_FLOOR_INIT;
int      idle_log_counter    = 0;

// ---------------------------------------------------------------------------
// Wi-Fi / I2S setup
// ---------------------------------------------------------------------------

void connectWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  const unsigned long timeout_ms = 30000;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeout_ms) {
      Serial.printf("\nWiFi connect timed out after %lu ms (status=%d). "
                    "Check SSID/password/2.4GHz reachability. Restarting...\n",
                    timeout_ms, (int)WiFi.status());
      delay(500);
      ESP.restart();
    }
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void setupI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("Mic ready");
}

// Try one allocation in PSRAM first, then in internal DRAM. Returns nullptr
// if neither fits.
//
// MALLOC_CAP_8BIT IS REQUIRED. Without it, the allocator may return memory
// from an IRAM-tagged pool that only supports 32-bit word-aligned access.
// Any later int16_t write or memset on such a buffer raises a LoadStoreError
// (EXCCAUSE 3, EXCVADDR in the 0x40080000+ range). We do byte/16-bit access
// on these buffers, so insist on 8-bit-accessible memory.
void* tryAllocBytes(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Allocate the PCM buffer, walking down 1 second at a time until something
// fits. Updates pcm_capacity_samples / pcm_capacity_bytes / pcm_capacity_seconds.
void allocatePcmBuffer() {
  Serial.printf("Heap at boot: free=%u, largest contiguous=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());

  for (int s = MAX_UTTERANCE_SECONDS; s >= 1; s--) {
    size_t samples = (size_t)SAMPLE_RATE * (size_t)s;
    size_t bytes   = samples * sizeof(int16_t);
    void*  p       = tryAllocBytes(bytes);
    if (p) {
      pcm_buffer            = (int16_t*)p;
      pcm_capacity_samples  = samples;
      pcm_capacity_bytes    = bytes;
      pcm_capacity_seconds  = s;
      Serial.printf("pcm_buffer: %u bytes (%d s) allocated. "
                    "Remaining free heap %u.\n",
                    (unsigned)bytes, s, (unsigned)ESP.getFreeHeap());
      if (s != MAX_UTTERANCE_SECONDS) {
        Serial.printf("NOTE: requested %d s but only %d s fit. "
                      "Long utterances will be split across multiple uploads.\n",
                      MAX_UTTERANCE_SECONDS, s);
      }
      return;
    }
    Serial.printf("  %d s (%u bytes) didn't fit, trying shorter...\n",
                  s, (unsigned)bytes);
  }

  Serial.printf("FATAL: could not allocate even 1 second of audio "
                "(free=%u, largest=%u). Enable PSRAM or free up DRAM.\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  while (true) { delay(1000); }
}

void allocatePrerollBuffer() {
  size_t bytes = PREROLL_TOTAL_SAMPLES * sizeof(int16_t);
  preroll_buf  = (int16_t*)tryAllocBytes(bytes);
  if (!preroll_buf) {
    Serial.printf("FATAL: could not allocate %u bytes for preroll_buf.\n",
                  (unsigned)bytes);
    while (true) { delay(1000); }
  }
  memset(preroll_buf, 0, bytes);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Allocate the small preroll buffer first so the larger PCM allocation
  // gets the biggest remaining contiguous DRAM block.
  allocatePrerollBuffer();
  allocatePcmBuffer();

  connectWifi();
  setupI2S();
  tls.setInsecure(); // prototype: skip cert validation

  Serial.printf("VAD ready. frame=%dms preroll=%dms hangover=%dms "
                "min_voice=%dms trigger_snr=%.1fx min_peak=%d max_utt=%ds\n",
                FRAME_MS, PREROLL_FRAMES * FRAME_MS,
                HANGOVER_FRAMES * FRAME_MS, MIN_VOICED_FRAMES * FRAME_MS,
                (double)TRIGGER_SNR, (int)ABSOLUTE_MIN_PEAK,
                pcm_capacity_seconds);
}

// ---------------------------------------------------------------------------
// Frame capture: read one I2S frame, convert to int16, return RMS + peak
// (FrameStats struct is declared near the top of the file.)
// ---------------------------------------------------------------------------

bool readFrame(int16_t* out, FrameStats* stats) {
  int32_t raw[FRAME_SAMPLES];
  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes_read, portMAX_DELAY);
  if (err != ESP_OK) {
    Serial.printf("i2s_read error: %d\n", err);
    return false;
  }
  size_t got = bytes_read / sizeof(int32_t);
  if (got == 0) return false;

  uint32_t peak   = 0;
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < got; i++) {
    int32_t s = raw[i] >> MIC_SHIFT;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    int16_t s16 = (int16_t)s;
    out[i] = s16;
    uint32_t mag = (s16 < 0) ? (uint32_t)(-s16) : (uint32_t)s16;
    if (mag > peak) peak = mag;
    sum_sq += (uint64_t)mag * mag;
  }
  // Pad if i2s_read returned a short read (rare).
  for (size_t i = got; i < FRAME_SAMPLES; i++) out[i] = 0;

  stats->peak = (uint16_t)((peak > 32767) ? 32767 : peak);
  stats->rms  = sqrtf((float)sum_sq / (float)got);
  return true;
}

// Push a frame into the circular preroll buffer.
void pushPreroll(const int16_t* frame) {
  size_t pos = preroll_pos;
  for (size_t i = 0; i < FRAME_SAMPLES; i++) {
    preroll_buf[pos++] = frame[i];
    if (pos >= PREROLL_TOTAL_SAMPLES) pos = 0;
  }
  preroll_pos = pos;
}

// Drain the preroll into pcm_buffer in chronological order.
size_t flushPrerollIntoPcm() {
  size_t dst = 0;
  size_t src = preroll_pos; // oldest sample lives at the write index
  for (size_t i = 0; i < PREROLL_TOTAL_SAMPLES; i++) {
    pcm_buffer[dst++] = preroll_buf[src++];
    if (src >= PREROLL_TOTAL_SAMPLES) src = 0;
  }
  return dst;
}

// ---------------------------------------------------------------------------
// Network: send the recorded utterance + log the response
// ---------------------------------------------------------------------------

void sendUtterance(size_t samples) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped, reconnecting...");
    connectWifi();
  }

  const size_t bytes_to_send = samples * sizeof(int16_t);

  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false);
  if (!http.begin(tls, server)) {
    Serial.println("http.begin() failed");
    return;
  }
  http.addHeader("Content-Type", "audio/pcm");
  http.addHeader("Accept", "application/json");

  Serial.printf("POST %u bytes (%.2f s)\n",
                (unsigned)bytes_to_send,
                (float)samples / (float)SAMPLE_RATE);

  unsigned long t0 = millis();
  int code = http.POST((uint8_t*)pcm_buffer, bytes_to_send);
  unsigned long elapsed = millis() - t0;
  String body = http.getString();

  Serial.printf("HTTP %d in %lu ms\n", code, elapsed);
  if (code <= 0) {
    Serial.printf("Transport error: %s\n",
                  HTTPClient::errorToString(code).c_str());
  }
  if (body.length() > 0) {
    Serial.println(body);
  } else {
    Serial.println("<empty body>");
  }

  int t_idx = body.indexOf("\"transcript\"");
  if (t_idx >= 0) {
    int colon = body.indexOf(':', t_idx);
    int q1 = body.indexOf('"', colon + 1);
    int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) {
      Serial.print("TRANSCRIPT: ");
      Serial.println(body.substring(q1 + 1, q2));
    }
  }
  http.end();
}

// ---------------------------------------------------------------------------
// VAD finalize helpers
// ---------------------------------------------------------------------------

void finalizeUtterance(const char* reason) {
  float voiced_ms = utt_voiced_frames * (float)FRAME_MS;
  float total_ms  = (utt_samples * 1000.0f) / (float)SAMPLE_RATE;

  if (utt_voiced_frames >= MIN_VOICED_FRAMES) {
    Serial.printf("[utterance] %s. voiced=%.0fms total=%.0fms -> uploading\n",
                  reason, voiced_ms, total_ms);
    sendUtterance(utt_samples);
  } else {
    Serial.printf("[utterance] %s. voiced=%.0fms total=%.0fms -> "
                  "DISCARDED (below %dms)\n",
                  reason, voiced_ms, total_ms,
                  MIN_VOICED_FRAMES * FRAME_MS);
  }

  vad_state           = VAD_IDLE;
  utt_samples         = 0;
  utt_voiced_frames   = 0;
  utt_hangover_frames = 0;
  idle_log_counter    = 0;
}

// ---------------------------------------------------------------------------
// Main loop: streaming VAD
// ---------------------------------------------------------------------------

void loop() {
  int16_t    frame[FRAME_SAMPLES];
  FrameStats st;
  if (!readFrame(frame, &st)) return;

  // Always update the rolling preroll so we can include audio that arrived
  // BEFORE the trigger when an utterance starts.
  pushPreroll(frame);

  const float trigger_rms = noise_floor_rms * TRIGGER_SNR;
  const float release_rms = noise_floor_rms * RELEASE_SNR;

  if (vad_state == VAD_IDLE) {
    bool loud_enough  = (st.peak >= ABSOLUTE_MIN_PEAK);
    bool above_noise  = (st.rms  >= trigger_rms);

    if (loud_enough && above_noise) {
      utt_samples         = flushPrerollIntoPcm();
      // Append the trigger frame too -- it's already in the preroll, so
      // skip re-appending. (Last FRAME_SAMPLES of pcm_buffer == this frame.)
      utt_voiced_frames   = 1;
      utt_hangover_frames = 0;
      vad_state           = VAD_RECORDING;
      Serial.printf("[trigger] rms=%.0f peak=%u  noise=%.0f  snr=%.1fx  "
                    "(preroll=%ums)\n",
                    st.rms, st.peak, noise_floor_rms,
                    st.rms / (noise_floor_rms + 1e-3f),
                    (unsigned)(PREROLL_FRAMES * FRAME_MS));
    } else {
      // Adapt noise floor only while idle.
      noise_floor_rms = (1.0f - NOISE_EMA_ALPHA) * noise_floor_rms +
                        NOISE_EMA_ALPHA * st.rms;
      if (noise_floor_rms < NOISE_FLOOR_MIN) noise_floor_rms = NOISE_FLOOR_MIN;
      if (noise_floor_rms > NOISE_FLOOR_MAX) noise_floor_rms = NOISE_FLOOR_MAX;

      if (++idle_log_counter >= NOISE_LOG_PERIOD) {
        idle_log_counter = 0;
        Serial.printf("[idle] noise_floor=%.0f  last_rms=%.0f  last_peak=%u\n",
                      noise_floor_rms, st.rms, st.peak);
      }
    }
    return;
  }

  // ---- VAD_RECORDING ----
  // Append this frame to the utterance buffer (with bounds check).
  if (utt_samples + FRAME_SAMPLES > pcm_capacity_samples) {
    finalizeUtterance("buffer full");
    return;
  }
  memcpy(&pcm_buffer[utt_samples], frame, FRAME_SAMPLES * sizeof(int16_t));
  utt_samples += FRAME_SAMPLES;

  if (st.rms >= release_rms || st.peak >= ABSOLUTE_MIN_PEAK) {
    utt_voiced_frames++;
    utt_hangover_frames = 0;
  } else {
    utt_hangover_frames++;
    if (utt_hangover_frames >= HANGOVER_FRAMES) {
      finalizeUtterance("end of speech");
    }
  }
}
