import { Hono } from 'hono'
import { cors } from 'hono/cors'
import { logger } from 'hono/logger'
import { HTTPException } from 'hono/http-exception'
import type { ContentfulStatusCode } from 'hono/utils/http-status'
import { generateText, stepCountIs, tool } from 'ai'
import { createOpenAI } from '@ai-sdk/openai'
import { APICallError } from '@ai-sdk/provider'
import Firecrawl from '@mendable/firecrawl-js'
import { z } from 'zod'

/**
 * Open EvE backend - voice agent for the desk robot.
 *
 * Endpoints:
 *   POST /transcribe  Audio -> text via Sarvam Saaras v3 STT (passthrough JSON: transcript + language_code).
 *   POST /chat        Text -> agent -> TTS (conditional):
 *                       Indian langs -> Sarvam Bulbul streaming `linear16` PCM.
 *                       English / international -> xAI `/v1/tts` PCM @ 24 kHz, voice `eve`.
 *                     KV history per `deviceId`.
 *
 * Supported request bodies on POST /transcribe:
 *   1. multipart/form-data with a `file` field (formats Sarvam accepts).
 *   2. A binary audio file body with Content-Type audio/wav | audio/mpeg |
 *      audio/aac | audio/flac | audio/ogg.
 *   3. Raw little-endian PCM with Content-Type audio/pcm | audio/L16 |
 *      application/octet-stream. Optional query params describe the PCM — Worker wraps WAV for Sarvam.
 *
 * Optional /transcribe query params:
 *   mode          transcribe (default) | translate | verbatim | translit | codemix — forwarded to Sarvam.
 *   language_code BCP-47 hint, e.g. hi-IN, en-IN. Forwarded when set.
 */

const SARVAM_STT_URL = 'https://api.sarvam.ai/speech-to-text'
const SARVAM_STT_MODEL = 'saaras:v3'
const XAI_TTS_URL = 'https://api.x.ai/v1/tts'
/** Streaming endpoint (binary audio body); `/chat` proxies PCM to ESP32 for Sarvam path. */
const SARVAM_TTS_STREAM_URL = 'https://api.sarvam.ai/text-to-speech/stream'
const SARVAM_TTS_MODEL = 'bulbul:v3'
/** Defaults aligned with Sarvam streaming hi-IN preset (ESP uses linear16, not mp3). */
const SARVAM_TTS_DEFAULT_SPEAKER = 'simran'
const SARVAM_TTS_DEFAULT_LANG = 'hi-IN'
/** Speech bandwidth: 8 kHz sounds telephone-muffled; 24000 Hz is a good ESP32/I2S tradeoff vs 24k. */
const SARVAM_TTS_SAMPLE_RATE = 24000
const SARVAM_TTS_PACE = 1.2
const SARVAM_TTS_ENABLE_PREPROCESSING = true
/** Must stay linear16 for Open EvE firmware I2S playback. */
const SARVAM_TTS_OUTPUT_CODEC = 'linear16' as const
const SARVAM_TTS_PCM_CHUNK_BYTES = 8192 // ReadableStream enqueue size (downstream chunked TE)
const SARVAM_TTS_CHAR_LIMIT = 2500 // bulbul:v3 caps at 2500; keep headroom for safety
/** xAI TTS allows up to ~15k chars; stay slightly under */
const XAI_TTS_CHAR_LIMIT = 14000
const DEFAULT_CHAT_LANGUAGE_CODE = 'en'
const XAI_TTS_VOICE_INTERNATIONAL = 'eve'
/** Keep total response headers within safe proxy limits (`x-reply-text` is URL-encoded). */
const CHAT_REPLY_HEADER_MAX_ENCODED_BYTES = 6144
const MAX_AUDIO_BYTES = 25 * 1024 * 1024 // 25 MB cap; Sarvam sync STT ~30 s practical limit
const MAX_HISTORY_MESSAGES = 50 // 6 user + 6 assistant turns
const HISTORY_TTL_SECONDS = 60 * 30 // 30 min idle session window
const OPENHORIZON_MODEL = 'openhorizon/gemma4:latest'

/** Primary ISO 639-1 tags treated as Indian languages → Sarvam TTS. */
const INDIAN_PRIMARY_LANG_TAGS = new Set([
  'as',
  'bn',
  'gu',
  'hi',
  'kn',
  'ml',
  'mr',
  'or',
  'pa',
  'ta',
  'te',
  'ur',
])

const VALID_MODES = ['transcribe', 'translate', 'verbatim', 'translit', 'codemix'] as const
type Mode = (typeof VALID_MODES)[number]

const app = new Hono<{ Bindings: CloudflareBindings }>()

app.use('*', logger())
app.use(
  '*',
  cors({
    origin: '*',
    allowMethods: ['GET', 'POST', 'OPTIONS'],
    allowHeaders: ['Content-Type', 'Authorization'],
    exposeHeaders: [
      'x-reply-text',
      'x-reply-truncated',
      'x-pcm-sample-rate',
      'x-history-len',
      'x-open-eve-envelope',
    ],
    maxAge: 86400,
  })
)

app.get('/', (c) =>
  c.json({
    name: 'open-eve-backend',
    description: 'Voice agent for the Open EvE desk robot (ESP32 + mic + BT speaker)',
    upstreams: {
      stt: 'sarvam.ai/speech-to-text (Saaras v3)',
      tts_intl: 'x.ai/v1/tts → PCM eve @ 24 kHz',
      tts_in: 'sarvam.ai/text-to-speech/stream (Bulbul v3 → PCM)',
      llm: `${OPENHORIZON_MODEL} via Vercel AI SDK`,
      websearch: 'firecrawl.dev /search',
    },
    endpoints: {
      'GET /': 'this index',
      'GET /health': 'health probe',
      'POST /transcribe': 'transcribe audio -> text',
      'POST /chat':
        'text -> agent -> TTS: Indian Sarvam PCM; English/intl xAI eve PCM @ 24 kHz; x-reply-text; optional ?envelope=1',
    },
  })
)

app.get('/health', (c) => c.json({ status: 'ok', timestamp: Date.now() }))

app.post('/transcribe', async (c) => {
  if (!bindingSecret(c.env.SARVAM_API_KEY)) {
    throw new HTTPException(500, { message: 'SARVAM_API_KEY is not configured on the server' })
  }

  const mode = (c.req.query('mode') ?? 'transcribe') as Mode
  if (!VALID_MODES.includes(mode)) {
    throw new HTTPException(400, {
      message: `Invalid mode '${mode}'. Allowed: ${VALID_MODES.join(', ')}`,
    })
  }
  const languageCode = c.req.query('language_code') || undefined

  const contentTypeHeader = (c.req.header('content-type') ?? '').toLowerCase()
  const baseContentType = contentTypeHeader.split(';')[0].trim()

  let audioBlob: Blob
  let filename: string

  if (baseContentType === 'multipart/form-data' || contentTypeHeader.includes('multipart/form-data')) {
    const form = await c.req.formData().catch(() => null)
    if (!form) {
      throw new HTTPException(400, { message: 'Failed to parse multipart/form-data body' })
    }
    const file = form.get('file')
    if (!(file instanceof File)) {
      throw new HTTPException(400, { message: "Missing 'file' field in multipart body" })
    }
    audioBlob = file
    filename = file.name || 'audio.bin'
  } else if (
    baseContentType === 'audio/pcm' ||
    baseContentType === 'audio/l16' ||
    baseContentType === 'application/octet-stream' ||
    baseContentType === ''
  ) {
    const sampleRate = parseIntParam(c.req.query('sample_rate'), 16000)
    const channels = parseIntParam(c.req.query('channels'), 1)
    const bitsPerSample = parseIntParam(c.req.query('bits_per_sample'), 16)

    if (![8, 16, 24, 32].includes(bitsPerSample)) {
      throw new HTTPException(400, { message: 'bits_per_sample must be 8, 16, 24, or 32' })
    }
    if (channels < 1 || channels > 2) {
      throw new HTTPException(400, { message: 'channels must be 1 or 2' })
    }
    if (sampleRate < 8000 || sampleRate > 24000) {
      throw new HTTPException(400, { message: 'sample_rate must be between 8000 and 24000' })
    }

    const buf = await c.req.arrayBuffer()
    if (buf.byteLength === 0) {
      throw new HTTPException(400, { message: 'Empty audio payload' })
    }
    if (buf.byteLength > MAX_AUDIO_BYTES) {
      throw new HTTPException(413, { message: `Payload too large (>${MAX_AUDIO_BYTES} bytes)` })
    }

    const wav = wrapPcmInWav(new Uint8Array(buf), { sampleRate, channels, bitsPerSample })
    audioBlob = new Blob([wav], { type: 'audio/wav' })
    filename = 'esp32-recording.wav'
  } else if (isAudioContentType(baseContentType)) {
    const buf = await c.req.arrayBuffer()
    if (buf.byteLength === 0) {
      throw new HTTPException(400, { message: 'Empty audio payload' })
    }
    if (buf.byteLength > MAX_AUDIO_BYTES) {
      throw new HTTPException(413, { message: `Payload too large (>${MAX_AUDIO_BYTES} bytes)` })
    }
    audioBlob = new Blob([buf], { type: baseContentType })
    filename = `audio.${guessExtension(baseContentType)}`
  } else {
    throw new HTTPException(415, {
      message:
        `Unsupported Content-Type '${contentTypeHeader}'. Use multipart/form-data, ` +
        'audio/wav, audio/mpeg, audio/aac, audio/flac, audio/ogg, audio/pcm, audio/L16, or application/octet-stream.',
    })
  }

  const sarvamForm = new FormData()
  sarvamForm.append('model', SARVAM_STT_MODEL)
  sarvamForm.append('mode', mode)
  if (languageCode) sarvamForm.append('language_code', languageCode)
  sarvamForm.append('file', audioBlob, filename)

  let upstream: Response
  try {
    upstream = await fetch(SARVAM_STT_URL, {
      method: 'POST',
      headers: {
        'api-subscription-key': bindingSecret(c.env.SARVAM_API_KEY),
      },
      body: sarvamForm,
    })
  } catch (err) {
    throw new HTTPException(502, {
      message: `Failed to reach Sarvam API: ${err instanceof Error ? err.message : String(err)}`,
    })
  }

  const rawBody = await upstream.text()
  const parsedBody = safeJsonParse(rawBody)

  if (!upstream.ok) {
    return c.json(
      {
        error: 'Sarvam API returned an error',
        upstream_status: upstream.status,
        upstream_body: parsedBody ?? rawBody,
      },
      upstream.status as ContentfulStatusCode
    )
  }

  return c.json(parsedBody ?? { raw: rawBody })
})

// ---------------------------------------------------------------------------
// /chat : agent -> conditional TTS (Sarvam Indian | xAI international) -> PCM
// ---------------------------------------------------------------------------

type ChatTurn = { role: 'user' | 'assistant'; content: string }

const ChatBodySchema = z.object({
  deviceId: z.string().min(1).max(64),
  text: z.string().min(1).max(2000),
  // BCP-47 from STT (`/transcribe`): steers TTS provider and voice.
  language_code: z.string().min(2).max(16).optional(),
  speaker: z.string().min(1).max(32).optional(),
  pace: z.number().min(0.5).max(2).optional(),
  enable_preprocessing: z.boolean().optional(),
  // Set true to wipe per-device history before this turn (e.g. wake word).
  reset: z.boolean().optional(),
})

app.post('/chat', async (c) => {
  const xaiKey = bindingSecret(c.env.XAI_API_KEY)
  const openHorizonApiKey = bindingSecret(c.env.OPENHORIZON_API_KEY)
  if (!openHorizonApiKey) {
    throw new HTTPException(500, { message: 'OPENHORIZON_API_KEY is not configured' })
  }
  if (!c.env.FIRECRAWL_API_KEY) {
    throw new HTTPException(500, { message: 'FIRECRAWL_API_KEY is not configured' })
  }
  if (!c.env.SESSIONS) {
    throw new HTTPException(500, { message: 'SESSIONS KV binding is missing' })
  }

  const raw = await c.req.json().catch(() => null)
  const parsed = ChatBodySchema.safeParse(raw)
  if (!parsed.success) {
    throw new HTTPException(400, {
      message: 'Invalid /chat body: ' + parsed.error.issues.map((i) => i.message).join('; '),
    })
  }
  const { deviceId, text, reset } = parsed.data
  const langRaw = (parsed.data.language_code ?? DEFAULT_CHAT_LANGUAGE_CODE).trim()
  const useIndianTts = isIndianLanguage(langRaw)
  if (useIndianTts) {
    if (!bindingSecret(c.env.SARVAM_API_KEY)) {
      throw new HTTPException(500, { message: 'SARVAM_API_KEY is not configured (required for Indian-language TTS)' })
    }
  } else if (!xaiKey) {
    throw new HTTPException(500, { message: 'XAI_API_KEY is not configured (required for non-Indian TTS)' })
  }

  const languageCode = langRaw.length >= 2 ? langRaw : DEFAULT_CHAT_LANGUAGE_CODE
  const ttsCharLimit = useIndianTts ? SARVAM_TTS_CHAR_LIMIT : XAI_TTS_CHAR_LIMIT
  const speaker = parsed.data.speaker ?? SARVAM_TTS_DEFAULT_SPEAKER
  const ttsPace = parsed.data.pace ?? SARVAM_TTS_PACE
  const ttsPreprocess =
    parsed.data.enable_preprocessing ?? SARVAM_TTS_ENABLE_PREPROCESSING
  const envelopeParam = (c.req.query('envelope') ?? '').toLowerCase()
  const useEnvelope =
    envelopeParam === '1' || envelopeParam === 'true' || envelopeParam === 'yes'

  const histKey = `hist:${deviceId}`
  const history = reset
    ? []
    : ((await c.env.SESSIONS.get<ChatTurn[]>(histKey, 'json')) ?? [])

  const openai = createOpenAI({
    baseURL: 'https://api.openhorizon.devwtf.in/v1',
    apiKey: openHorizonApiKey,
  })
  const firecrawl = new Firecrawl({ apiKey: c.env.FIRECRAWL_API_KEY })

  const webSearch = tool({
    description:
      'Search the live web for up-to-date facts (news, weather, prices, sports, anything time-sensitive). ' +
      'Use only when the user asks for something the model cannot reliably know from training data.',
    inputSchema: z.object({
      query: z.string().min(2).max(200).describe('Concise search query in English or the user language.'),
      limit: z.number().int().min(1).max(4).default(3),
    }),
    execute: async ({ query, limit }) => {
      try {
        const res = await firecrawl.search(query, {
          limit,
          scrapeOptions: { formats: ['markdown'], onlyMainContent: true },
        })
        const items = (res.web ?? []) as Array<{
          url?: string
          title?: string
          description?: string
          markdown?: string
        }>
        return items.slice(0, limit).map((r) => ({
          title: r.title ?? '',
          url: r.url ?? '',
          snippet: ((r.markdown ?? r.description ?? '') || '').slice(0, 1200),
        }))
      } catch (err) {
        return {
          error: 'web_search_failed',
          detail: err instanceof Error ? err.message : String(err),
        }
      }
    },
  })

  const systemPrompt =
    "You are EvE, a friendly desk robot by Saidev Dhal built using OpenHorizon AI provider. Reply in 1 to 3 short sentences " +
    "suitable for spoken audio. Never use markdown, bullet lists, code fences, " +
    "URLs, or emojis - the reply will be read out loud by a TTS engine. " +
    "If a fact is time sensitive (today, latest, current, live, now) call the " +
    "webSearch tool first and cite the answer succinctly. " +
    "Speak the same language the user used. " +
    `Hard limit: never exceed ${ttsCharLimit} characters.`

  let reply: string
  try {
    const result = await generateText({
      // Use Chat Completions (`/v1/chat/completions`). The callable `openai(...)`
      // defaults to the Responses API (`/v1/responses`), which OpenHorizon does not expose.
      model: openai.chat(OPENHORIZON_MODEL),
      system: systemPrompt,
      messages: [...history, { role: 'user', content: text }],
      tools: { webSearch },
      stopWhen: stepCountIs(3),
      temperature: 0.4,
    })
    reply = result.text.trim()
  } catch (err) {
    console.error('agent generateText failed:', err)
    throw new HTTPException(502, {
      message: 'Agent failed: ' + formatAgentError(err),
    })
  }

  if (!reply) reply = 'Sorry, I did not catch that. Could you say it again?'
  if (reply.length > ttsCharLimit) reply = reply.slice(0, ttsCharLimit)

  const newHist: ChatTurn[] = [
    ...history,
    { role: 'user' as const, content: text },
    { role: 'assistant' as const, content: reply },
  ].slice(-MAX_HISTORY_MESSAGES)

  c.executionCtx.waitUntil(
    c.env.SESSIONS.put(histKey, JSON.stringify(newHist), { expirationTtl: HISTORY_TTL_SECONDS })
  )

  const pcmHeaders = chatPcmResponseHeaders(reply, newHist.length, useEnvelope, SARVAM_TTS_SAMPLE_RATE)

  if (!useIndianTts) {
    const ttsBody = {
      text: reply,
      voice_id: XAI_TTS_VOICE_INTERNATIONAL,
      language: languageForXaiTts(languageCode),
      output_format: {
        codec: 'pcm',
        sample_rate: SARVAM_TTS_SAMPLE_RATE,
      },
    }
    let ttsResp: Response
    try {
      ttsResp = await fetch(XAI_TTS_URL, {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${xaiKey}`,
          'content-type': 'application/json',
        },
        body: JSON.stringify(ttsBody),
      })
    } catch (err) {
      throw new HTTPException(502, {
        message: 'Failed to reach xAI TTS: ' + (err instanceof Error ? err.message : String(err)),
      })
    }
    if (!ttsResp.ok) {
      const detail = await ttsResp.text()
      throw new HTTPException(502, { message: `xAI TTS error ${ttsResp.status}: ${detail}` })
    }
    const rawPcm = new Uint8Array(await ttsResp.arrayBuffer())
    const pcm = stripWavHeaderIfPresent(rawPcm)
    let stream: ReadableStream<Uint8Array> = streamUint8ArrayInChunks(pcm, SARVAM_TTS_PCM_CHUNK_BYTES)
    if (useEnvelope) {
      stream = prependUint8ThenReadable(pcmEnvelopePrefix(reply), stream)
    }
    return new Response(stream, { status: 200, headers: pcmHeaders })
  }

  const sarvamKey = bindingSecret(c.env.SARVAM_API_KEY)
  const ttsPayload = {
    text: reply,
    target_language_code: languageCode.includes('-') ? languageCode : `${primaryLanguageSubtag(languageCode)}-IN`,
    model: SARVAM_TTS_MODEL,
    speaker,
    speech_sample_rate: SARVAM_TTS_SAMPLE_RATE,
    pace: ttsPace,
    enable_preprocessing: ttsPreprocess,
    output_audio_codec: SARVAM_TTS_OUTPUT_CODEC,
  }

  let ttsResp: Response
  try {
    ttsResp = await fetch(SARVAM_TTS_STREAM_URL, {
      method: 'POST',
      headers: {
        'api-subscription-key': sarvamKey,
        'content-type': 'application/json',
      },
      body: JSON.stringify(ttsPayload),
    })
  } catch (err) {
    throw new HTTPException(502, {
      message: 'Failed to reach Sarvam TTS stream: ' + (err instanceof Error ? err.message : String(err)),
    })
  }

  if (!ttsResp.ok) {
    const detail = await ttsResp.text()
    throw new HTTPException(502, { message: `Sarvam TTS stream error ${ttsResp.status}: ${detail}` })
  }

  const upstreamCt = (ttsResp.headers.get('content-type') ?? '').toLowerCase()

  if (upstreamCt.includes('application/json')) {
    const ttsJson = (await ttsResp.json().catch(() => null)) as { audios?: string[] } | null
    if (!ttsJson?.audios?.length) {
      throw new HTTPException(502, { message: 'Sarvam TTS returned JSON without audio' })
    }
    const pcm = pcmPayloadFromTtsAudios(ttsJson.audios)
    let stream: ReadableStream<Uint8Array> = streamUint8ArrayInChunks(pcm, SARVAM_TTS_PCM_CHUNK_BYTES)
    if (useEnvelope) {
      stream = prependUint8ThenReadable(pcmEnvelopePrefix(reply), stream)
    }
    return new Response(stream, {
      status: 200,
      headers: pcmHeaders,
    })
  }

  if (!ttsResp.body) {
    throw new HTTPException(502, { message: 'Sarvam TTS stream returned no body' })
  }

  let outBody: ReadableStream<Uint8Array> = ttsResp.body
  if (useEnvelope) {
    outBody = prependUint8ThenReadable(pcmEnvelopePrefix(reply), outBody)
  }

  return new Response(outBody, {
    status: 200,
    headers: pcmHeaders,
  })
})

app.onError((err, c) => {
  if (err instanceof HTTPException) {
    return c.json({ error: err.message }, err.status)
  }
  console.error('Unhandled error:', err)
  return c.json({ error: 'Internal server error', details: String(err) }, 500)
})

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function primaryLanguageSubtag(languageCodeHint: string): string {
  return languageCodeHint.trim().split(/[-_]/)[0].toLowerCase()
}

function isIndianLanguage(languageCodeHint: string): boolean {
  return INDIAN_PRIMARY_LANG_TAGS.has(primaryLanguageSubtag(languageCodeHint))
}

/** Map user/STT language hint to xAI TTS `language` (BCP-ish / `auto`). */
function languageForXaiTts(languageCodeHint: string): string {
  const raw = languageCodeHint.trim().toLowerCase()
  const p = primaryLanguageSubtag(raw)
  if (p === 'en' || raw.startsWith('en-')) return 'en'
  if (p === 'zh' || raw.startsWith('zh-')) return 'zh'
  const direct: Record<string, string> = {
    fr: 'fr',
    de: 'de',
    ja: 'ja',
    ko: 'ko',
    ru: 'ru',
    hi: 'hi',
    bn: 'bn',
    it: 'it',
    id: 'id',
    tr: 'tr',
    vi: 'vi',
    es: 'es-ES',
    pt: 'pt-BR',
    ar: 'ar-SA',
  }
  return direct[p] ?? 'auto'
}

/** Trim Worker secrets — pasted keys often include accidental newlines. */
function bindingSecret(value: string | undefined): string {
  return String(value ?? '').trim()
}

function formatAgentError(err: unknown): string {
  if (APICallError.isInstance(err)) {
    const bits: string[] = [err.message]
    const body = err.responseBody?.trim()
    if (body && body.length > 0 && body.length <= 800 && !bits[0].includes(body)) {
      bits.push(body)
    }
    const unauthorized =
      err.statusCode === 401 || /\bunauthorized\b/i.test(bits.join(' '))
    if (unauthorized) {
      bits.push(
        'OpenHorizon expects Authorization: Bearer <key> (the AI SDK does this automatically). ' +
          'Update the key with wrangler secret put OPENHORIZON_API_KEY (prod) or add OPENHORIZON_API_KEY=… to .dev.vars for wrangler dev — Wrangler does not read .env.'
      )
    }
    return bits.join(' ')
  }
  return err instanceof Error ? err.message : String(err)
}

function parseIntParam(value: string | undefined, fallback: number): number {
  if (value === undefined || value === '') return fallback
  const n = Number.parseInt(value, 10)
  return Number.isFinite(n) ? n : fallback
}

function isAudioContentType(ct: string): boolean {
  return (
    ct === 'audio/wav' ||
    ct === 'audio/x-wav' ||
    ct === 'audio/wave' ||
    ct === 'audio/mpeg' ||
    ct === 'audio/mp3' ||
    ct === 'audio/aac' ||
    ct === 'audio/flac' ||
    ct === 'audio/x-flac' ||
    ct === 'audio/ogg' ||
    ct === 'audio/opus'
  )
}

function guessExtension(contentType: string): string {
  if (contentType.includes('wav')) return 'wav'
  if (contentType.includes('mpeg') || contentType.includes('mp3')) return 'mp3'
  if (contentType.includes('aac')) return 'aac'
  if (contentType.includes('flac')) return 'flac'
  if (contentType.includes('ogg') || contentType.includes('opus')) return 'ogg'
  return 'bin'
}

function safeJsonParse(text: string): unknown | null {
  try {
    return JSON.parse(text)
  } catch {
    return null
  }
}

interface PcmInfo {
  sampleRate: number
  channels: number
  bitsPerSample: number
}

/**
 * Build a 44-byte canonical PCM WAV file in front of the supplied samples.
 * Sarvam rejects raw PCM; the ESP32 streams PCM and the Worker wraps it here.
 */
function wrapPcmInWav(pcm: Uint8Array, info: PcmInfo): Uint8Array {
  const { sampleRate, channels, bitsPerSample } = info
  const byteRate = (sampleRate * channels * bitsPerSample) / 8
  const blockAlign = (channels * bitsPerSample) / 8
  const dataSize = pcm.byteLength

  const buffer = new ArrayBuffer(44 + dataSize)
  const view = new DataView(buffer)

  writeString(view, 0, 'RIFF')
  view.setUint32(4, 36 + dataSize, true)
  writeString(view, 8, 'WAVE')

  writeString(view, 12, 'fmt ')
  view.setUint32(16, 16, true) // fmt chunk size for PCM
  view.setUint16(20, 1, true) // PCM format
  view.setUint16(22, channels, true)
  view.setUint32(24, sampleRate, true)
  view.setUint32(28, byteRate, true)
  view.setUint16(32, blockAlign, true)
  view.setUint16(34, bitsPerSample, true)

  writeString(view, 36, 'data')
  view.setUint32(40, dataSize, true)

  new Uint8Array(buffer, 44).set(pcm)
  return new Uint8Array(buffer)
}

function writeString(view: DataView, offset: number, str: string): void {
  for (let i = 0; i < str.length; i++) {
    view.setUint8(offset + i, str.charCodeAt(i))
  }
}

/**
 * Decode base64 to bytes (Workers `atob`; no Node Buffer).
 */
function decodeBase64Binary(b64: string): Uint8Array {
  const bin = atob(b64)
  const out = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
  return out
}

function readFourCC(d: DataView, absoluteByteOffset: number): string {
  let s = ''
  for (let i = 0; i < 4; i++) s += String.fromCharCode(d.getUint8(absoluteByteOffset + i))
  return s
}

/** If Sarvam returns WAV despite `linear16`, return only the `data` chunk payload. */
function stripWavHeaderIfPresent(input: Uint8Array): Uint8Array {
  if (input.byteLength < 12) return input
  const abs0 = input.byteOffset
  const d = new DataView(input.buffer)
  if (readFourCC(d, abs0) !== 'RIFF' || readFourCC(d, abs0 + 8) !== 'WAVE') return input

  let o = abs0 + 12
  const absEnd = abs0 + input.byteLength
  while (o + 8 <= absEnd) {
    const id = readFourCC(d, o)
    const chunkSize = d.getUint32(o + 4, true)
    const dataAbs = o + 8
    const nextChunk = dataAbs + chunkSize + (chunkSize & 1)
    if (id === 'data') {
      const rel0 = dataAbs - abs0
      const rel1 = Math.min(dataAbs + chunkSize - abs0, input.byteLength)
      return input.subarray(rel0, rel1)
    }
    o = nextChunk
  }
  return input
}

function pcmPayloadFromTtsAudios(audios: string[]): Uint8Array {
  const parts: Uint8Array[] = []
  let total = 0
  for (const b64 of audios) {
    const raw = decodeBase64Binary(b64)
    const pcm = stripWavHeaderIfPresent(raw)
    parts.push(pcm)
    total += pcm.byteLength
  }
  const out = new Uint8Array(total)
  let offset = 0
  for (const p of parts) {
    out.set(p, offset)
    offset += p.byteLength
  }
  return out
}

/** Chunked Transfer-Encoding to the client; avoids one huge body write. */
function streamUint8ArrayInChunks(buffer: Uint8Array, chunkSize: number): ReadableStream<Uint8Array> {
  let offset = 0
  return new ReadableStream({
    pull(controller) {
      if (offset >= buffer.byteLength) {
        controller.close()
        return
      }
      const end = Math.min(offset + chunkSize, buffer.byteLength)
      controller.enqueue(buffer.subarray(offset, end))
      offset = end
    },
  })
}

function encodeReplyForHeader(reply: string, maxEncodedBytes: number): { value: string; truncated: boolean } {
  let n = reply.length
  while (n > 0) {
    const enc = encodeURIComponent(reply.slice(0, n))
    if (enc.length <= maxEncodedBytes) {
      return { value: enc, truncated: n < reply.length }
    }
    n -= 1
  }
  return { value: '', truncated: reply.length > 0 }
}

/** Optional body prefix when `POST /chat?envelope=1`: BE uint32 JSON byte length + UTF-8 `{"reply":"..."}`. */
function pcmEnvelopePrefix(reply: string): Uint8Array {
  const jsonBytes = new TextEncoder().encode(JSON.stringify({ reply }))
  const out = new Uint8Array(4 + jsonBytes.byteLength)
  new DataView(out.buffer).setUint32(0, jsonBytes.byteLength, false)
  out.set(jsonBytes, 4)
  return out
}

function prependUint8ThenReadable(
  prefix: Uint8Array,
  body: ReadableStream<Uint8Array>
): ReadableStream<Uint8Array> {
  const reader = body.getReader()
  let prefixSent = false
  return new ReadableStream({
    pull(controller) {
      return (async () => {
        if (!prefixSent) {
          controller.enqueue(prefix)
          prefixSent = true
          return
        }
        const { done, value } = await reader.read()
        if (done) {
          controller.close()
          return
        }
        if (value.byteLength) controller.enqueue(value)
      })()
    },
    cancel(reason) {
      return reader.cancel(reason)
    },
  })
}

function chatPcmResponseHeaders(
  reply: string,
  historyLen: number,
  envelope: boolean,
  pcmSampleRate: number
): HeadersInit {
  const { value, truncated } = encodeReplyForHeader(reply, CHAT_REPLY_HEADER_MAX_ENCODED_BYTES)
  const headers: Record<string, string> = {
    'content-type': 'application/octet-stream',
    'cache-control': 'no-cache',
    'x-pcm-sample-rate': String(pcmSampleRate),
    'x-reply-text': value,
    'x-history-len': String(historyLen),
  }
  if (truncated) headers['x-reply-truncated'] = '1'
  if (envelope) headers['x-open-eve-envelope'] = 'json-length-pcm-v1'
  return headers
}

export default app
