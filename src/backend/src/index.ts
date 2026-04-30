import { Hono } from 'hono'
import { cors } from 'hono/cors'
import { logger } from 'hono/logger'
import { HTTPException } from 'hono/http-exception'
import type { ContentfulStatusCode } from 'hono/utils/http-status'

/**
 * Open EvE backend - speech-to-text proxy.
 *
 * The desk robot (ESP32 + I2S mic) records short utterances and POSTs the
 * audio to this Worker. The Worker forwards the audio to Sarvam Saaras v3
 * (https://docs.sarvam.ai/api-reference-docs/api-guides-tutorials/speech-to-text/rest-api)
 * and returns the resulting transcript.
 *
 * Supported request bodies on POST /transcribe:
 *   1. multipart/form-data with a `file` field (any Sarvam-supported format).
 *   2. A binary audio file body with Content-Type audio/wav | audio/mpeg |
 *      audio/aac | audio/flac | audio/ogg.
 *   3. Raw little-endian PCM with Content-Type audio/pcm | audio/L16 |
 *      application/octet-stream. Optional query params describe the PCM:
 *        sample_rate     (default 16000)
 *        channels        (default 1)
 *        bits_per_sample (default 16)
 *      The Worker wraps the PCM in a WAV header before forwarding. This is
 *      the easiest path for an ESP32 streaming I2S samples directly.
 *
 * Optional query params:
 *   mode          transcribe (default) | translate | verbatim | translit | codemix
 *   language_code BCP-47 hint, e.g. hi-IN, en-IN. Required only when Sarvam
 *                 cannot auto-detect (mostly relevant for `transcribe` mode).
 */

const SARVAM_STT_URL = 'https://api.sarvam.ai/speech-to-text'
const SARVAM_MODEL = 'saaras:v3'
const MAX_AUDIO_BYTES = 25 * 1024 * 1024 // 25 MB safety cap; Sarvam sync limit is 30 s of audio

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
    maxAge: 86400,
  })
)

app.get('/', (c) =>
  c.json({
    name: 'open-eve-backend',
    description: 'Speech-to-text proxy for the Open EvE desk robot (ESP32 + mic)',
    upstream: 'sarvam.ai/speech-to-text (Saaras v3)',
    endpoints: {
      'GET /': 'this index',
      'GET /health': 'health probe',
      'POST /transcribe': 'transcribe audio -> text',
    },
  })
)

app.get('/health', (c) => c.json({ status: 'ok', timestamp: Date.now() }))

app.post('/transcribe', async (c) => {
  if (!c.env.SARVAM_API_KEY) {
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
    if (sampleRate < 8000 || sampleRate > 48000) {
      throw new HTTPException(400, { message: 'sample_rate must be between 8000 and 48000' })
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
  sarvamForm.append('model', SARVAM_MODEL)
  sarvamForm.append('mode', mode)
  if (languageCode) sarvamForm.append('language_code', languageCode)
  sarvamForm.append('file', audioBlob, filename)

  let upstream: Response
  try {
    upstream = await fetch(SARVAM_STT_URL, {
      method: 'POST',
      headers: {
        'api-subscription-key': c.env.SARVAM_API_KEY,
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

app.onError((err, c) => {
  if (err instanceof HTTPException) {
    return c.json({ error: err.message }, err.status)
  }
  console.error('Unhandled error:', err)
  return c.json({ error: 'Internal server error', details: String(err) }, 500)
})

export default app

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
 * Sarvam's REST API rejects raw PCM, so we wrap it before forwarding.
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
