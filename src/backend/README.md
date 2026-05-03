# Open EvE backend

Cloudflare Worker (Hono) that powers the Open EvE desk robot. It exposes:

- `POST /transcribe` – proxy to [xAI Speech to Text](https://docs.x.ai/developers/model-capabilities/audio/speech-to-text). The Worker normalizes the JSON to `transcript` + `language_code` for the ESP32 client.
- `POST /chat` – Vercel AI SDK agent (OpenHorizon + Firecrawl `webSearch` tool). After `generateText`, TTS is **conditional**:
  - **Indian languages** (`hi`, `bn`, `ta`, `te`, `kn`, `ml`, `mr`, `gu`, `pa`, `ur`, `or`, `as` per primary subtag): [Sarvam Bulbul](https://docs.sarvam.ai/api-reference-docs/api-guides-tutorials/text-to-speech/rest-api) streaming **`linear16`** PCM @ 24 kHz.
  - **All other languages** (including English and `en-*`): [xAI Text to Speech](https://docs.x.ai/developers/model-capabilities/audio/text-to-speech), voice **`eve`**, raw **PCM** @ 24 kHz.

The ESP32 streams mono s16le audio to the Worker (see [`src/speak/`](../speak/)).

## Run locally

```sh
bun install
bun run dev      # http://localhost:8787
```

Secrets are read from `.dev.vars` for local dev. For production each one is stored as a Worker secret:

```sh
bunx wrangler secret put XAI_API_KEY
bunx wrangler secret put SARVAM_API_KEY
bunx wrangler secret put OPENHORIZON_API_KEY
bunx wrangler secret put FIRECRAWL_API_KEY
bun run deploy
```

- **`XAI_API_KEY`** – required for `/transcribe` and for `/chat` when the utterance language is **not** in the Indian set above.
- **`SARVAM_API_KEY`** – required only when `/chat` routes to Bulbul (**Indian** `language_code`).

The KV namespace `SESSIONS` (used by `/chat` for conversation history) is created once with:

```sh
bunx wrangler kv namespace create SESSIONS
bunx wrangler kv namespace create SESSIONS --preview
# Paste the returned ids into wrangler.jsonc (already done in this repo).
```

After changing `wrangler.jsonc` or `.dev.vars`, regenerate types (**note:** if `wrangler types` emits wrong secret names vs this repo, ensure `worker-configuration.d.ts` matches the secrets used in `src/index.ts`):

```sh
bun run cf-typegen
```

## API

### `GET /`

Returns a JSON index of available endpoints.

### `GET /health`

`{ "status": "ok", "timestamp": <ms> }`

### `POST /transcribe`

Forwards WAV (or multipart file) to xAI **`POST https://api.x.ai/v1/stt`** and responds with normalized JSON:

```json
{
  "transcript": "Hello, how are you?",
  "language_code": "en"
}
```

`language_code` is derived from xAI’s detected language name (`English`, `Hindi`, … → BCP‑ish hints such as `en`, `hi-IN`). Unknown names fall back to `en`.

#### Query parameters

| Param           | Default       | Notes                                                                 |
| --------------- | ------------- | --------------------------------------------------------------------- |
| `mode`          | `transcribe`  | **Legacy.** Same values as the old Sarvam API are accepted but **ignored** (xAI has no equivalent). |
| `language_code` | _(omit)_      | Optional. When set, sent as xAI `language` with `format=true` (spoken-number normalization). Primary subtag used (e.g. `hi` from `hi-IN`). |
| `sample_rate`   | `16000`       | Only when sending raw PCM.                                          |
| `channels`      | `1`           | Only when sending raw PCM.                                          |
| `bits_per_sample` | `16`        | Only when sending raw PCM (8/16/24/32).                             |

#### Accepted request bodies

The Worker auto-detects the body shape from `Content-Type`:

1. **`multipart/form-data`** — standard upload with a `file` field (formats xAI accepts, e.g. WAV, MP3).
2. **A binary audio file** — set `Content-Type` to `audio/wav`, `audio/mpeg`, `audio/aac`, `audio/flac`, or `audio/ogg` and send the file as the body.
3. **Raw little-endian PCM** — set `Content-Type` to `audio/pcm`, `audio/L16`, or `application/octet-stream`. The Worker wraps the samples in a WAV header before forwarding. This matches the ESP32 I2S capture path.

Multipart field order follows xAI: non-file fields first, **`file` last**.

#### cURL examples

```sh
# multipart upload
curl -X POST 'http://localhost:8787/transcribe?language_code=en' \
  -F 'file=@recording.wav;type=audio/wav'

# Legacy mode flag (no-op, still allowed)
curl -X POST 'http://localhost:8787/transcribe?language_code=hi-IN&mode=translate' \
  -H 'Content-Type: audio/wav' \
  --data-binary @recording.wav

# raw PCM (16 kHz, 16-bit, mono) — what the ESP32 sends
curl -X POST 'http://localhost:8787/transcribe?sample_rate=16000&channels=1&bits_per_sample=16' \
  -H 'Content-Type: audio/pcm' \
  --data-binary @recording.pcm
```

### `POST /chat`

Runs the AI agent and returns **PCM s16le mono** @ **24 kHz** (`application/octet-stream`). Indian languages use Sarvam streaming Bulbul; other languages use a single buffered xAI TTS PCM response.

#### Request body (JSON)

| Field           | Required | Default   | Notes                                                                 |
| --------------- | -------- | --------- | --------------------------------------------------------------------- |
| `deviceId`      | yes      | –         | Stable id per robot (use the MAC). KV history key.                    |
| `text`          | yes      | –         | User transcript for this turn (1–2000 chars).                         |
| `language_code` | no       | `en`      | From `/transcribe` (recommended). Routes TTS: Indian set → Sarvam, else xAI `eve`. |
| `speaker`       | no       | `simran`  | Sarvam Bulbul speaker (**Indian branch only**).                       |
| `pace`          | no       | `1.2`     | Sarvam pace (Indian branch).                                          |
| `enable_preprocessing` | no | `true` | Sarvam preprocessing (Indian branch).                          |
| `reset`         | no       | `false`   | Clear the device's KV history before this turn.                       |

#### Response

`200` with `Content-Type: application/octet-stream` (chunked PCM). Reply text is in `x-reply-text` (URL-encoded). `x-pcm-sample-rate` is **`24000`**. Optional `POST /chat?envelope=1` adds the JSON-length prefix consumed by alternate clients.

Steps:

1. Load history from KV (`hist:<deviceId>`).
2. `generateText` with `webSearch` tool (`stopWhen: stepCountIs(3)`).
3. Trim assistant text to the active TTS limit (Sarvam **2500** chars; xAI **14000** chars).
4. Write history to KV (TTL 30 minutes).
5. **Indian** → Sarvam Bulbul streaming (same pipeline as before). **International** → `POST https://api.x.ai/v1/tts` with `voice_id: eve`, PCM @ 24 kHz; WAV header stripped if present.

#### Curl examples

```sh
curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","language_code":"en","text":"Tell me a one-line joke about robots."}' \
  -o reply.pcm -D headers.txt

curl -sS -X POST 'http://localhost:8787/chat' \
  -H 'content-type: application/json' \
  -d '{"deviceId":"esp32-test","language_code":"hi-IN","text":"आज मौसम कैसा है?"}' \
  -o reply_hi.pcm
```

## ESP32 client (Arduino sketch)

Typical mic: I2S MEMS at **16 kHz**, 16-bit mono. Capture one utterance, then POST raw PCM:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

const char* STT_URL    = "https://backend.example.workers.dev/transcribe"
                         "?sample_rate=16000&channels=1&bits_per_sample=16";
// Optional: "&language_code=hi-IN" — enables xAI format normalization for that language

String transcribe(const uint8_t* pcm_buf, size_t pcm_len) {
  HTTPClient http;
  http.begin(STT_URL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(20000);

  int status = http.POST(const_cast<uint8_t*>(pcm_buf), pcm_len);
  String body = http.getString();
  http.end();

  Serial.printf("STT %d: %s\n", status, body.c_str());
  return body; // JSON: { transcript, language_code }
}
```

Tips:

- Use PSRAM when available for long captures; tune `STT_MAX_CAPTURE_SAMPLES` vs heap.
- `WiFi.setSleep(false)` before large uploads; HTTPS with `WiFiClientSecure`.

## Project layout

```
src/backend/
├── src/index.ts            # Hono: /transcribe (xAI STT) + /chat + /health
├── wrangler.jsonc          # KV binding, nodejs_compat, observability
├── worker-configuration.d.ts  # typings (sync with Worker secrets used in index.ts)
├── .dev.vars               # local secrets (gitignored)
└── package.json
```

## Pipeline (full request)

```
ESP32 mic --(VAD)--> POST /transcribe (pcm -> WAV) --> xAI STT --> transcript + language_code
                                                          |
ESP32 (I2S speaker) <-- PCM 24 kHz <-- POST /chat (deviceId, text, language_code)
                                          |
                                          +-- KV: load history
                                          +-- AI SDK generateText (OpenHorizon) + Firecrawl webSearch
                                          +-- KV: save history (TTL 30 min)
                                          +-- TTS branch:
                                                Indian -> Sarvam Bulbul streaming linear16
                                                else   -> xAI TTS eve, PCM strip
```
