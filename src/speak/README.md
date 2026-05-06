# Open EvE — `speak.ino`

Voice loop on ESP32-S3: I2S mic → Worker STT → Worker chat/TTS → I2S speaker. Wi‑Fi + optional captive portal.

---

## Hardware wiring (everything the sketch actually uses)

The firmware only assigns GPIO for **two I2S buses**, an **optional wake button**, and **USB UART** for logs. There are **no other wired peripherals** in code (no LEDs, no SPI display, no Bluetooth audio).

Pin defines and `i2s_set_pin` usage:

```102:110:src/speak/speak.ino
// ===== MIC (I2S0) =====
#define I2S_WS    15
#define I2S_SCK   16
#define I2S_SD    4

// ===== SPEAKER (I2S1) =====
#define I2S_OUT_WS   17
#define I2S_OUT_SCK  18
#define I2S_OUT_SD   21
```

I2S roles from `setupI2SMic()` / `setupI2SSpeaker()`: ESP32 is **I2S master** on both ports — it drives **WS** and **SCK** for the mic and for the speaker; it **reads** mic data on `I2S_SD` and **writes** speaker data on `I2S_OUT_SD`.

### 1. I2S microphone → ESP32-S3

Wire the mic breakout to the DevKit like this:

| Mic breakout pin (typical names) | Connect to ESP32-S3 | `speak.ino` define |
| --------------------------------- | ------------------- | ------------------ |
| **WS** / LRCL / LRCK | **GPIO 15** | `I2S_WS` |
| **SCK** / BCLK | **GPIO 16** | `I2S_SCK` |
| **SD** / DOUT (data from mic) | **GPIO 4** | `I2S_SD` |
| **VDD** / 3V3 | **3.3 V** on the board | — |
| **GND** | **GND** | — |

Notes from the firmware (affects module straps, not more GPIO):

- Mic path is **I2S RX**, **16 kHz**, **left channel only** (`I2S_CHANNEL_FMT_ONLY_LEFT` in `setupI2SMic()`).
- On parts like the **INMP441**, set **L/R** to match **left** for that channel choice (often **GND** or **3V3** per datasheet).

### 2. I2S speaker / DAC+amp → ESP32-S3

Wire the amplifier (e.g. MAX98357-style) to the **same** ESP32:

| Amp breakout pin (typical names) | Connect to ESP32-S3 | `speak.ino` define |
| --------------------------------- | ------------------- | ------------------ |
| **LRC** / WS (word select) | **GPIO 17** | `I2S_OUT_WS` |
| **BCLK** | **GPIO 18** | `I2S_OUT_SCK` |
| **DIN** (data **into** amp) | **GPIO 21** | `I2S_OUT_SD` |
| **VIN** / **VDD** | **3.3 V** or **5 V** per module datasheet | — |
| **GND** | **GND** | — |

Firmware side: **I2S TX**, **24 kHz**, **left channel only** (`setupI2SSpeaker()` — matches Worker TTS rate in comments).

If your module has **SD** / **GAIN** pins, follow the breakout’s datasheet (not referenced in `speak.ino`).

### 3. Optional hardware wake button (compile-time only)

Default in code:

```94:96:src/speak/speak.ino
#ifndef OPEN_EVE_HARDWARE_WAKE_GPIO
#define OPEN_EVE_HARDWARE_WAKE_GPIO -1
#endif
```

`-1` means **no wake GPIO** — this block is compiled out.

If you change the sketch (or build flag) so `OPEN_EVE_HARDWARE_WAKE_GPIO` is **≥ 0**, that number becomes a real GPIO:

```1405:1408:src/speak/speak.ino
#if OPEN_EVE_HARDWARE_WAKE_GPIO >= 0
  pinMode(OPEN_EVE_HARDWARE_WAKE_GPIO, INPUT_PULLUP);
  Serial.printf("[CONFIG] Hardware wake GPIO %d (active LOW) — idle audio stays offline\n",
                OPEN_EVE_HARDWARE_WAKE_GPIO);
```

Wiring:

| Part | Connection |
| ---- | ---------- |
| Momentary switch | One leg → **GPIO `OPEN_EVE_HARDWARE_WAKE_GPIO`**, other leg → **GND** |
| (internal) | `INPUT_PULLUP` — **unpressed** = high, **pressed** = low |

Pick any free GPIO you assign in code; there is no default pin while it stays `-1`.

### 4. Power + serial (host)

| Purpose | Hardware |
| ------- | -------- |
| Run + flash + `Serial` logs | **USB** to the DevKit (`Serial.begin(115200)` in `setup()`) |
| Supply | DevKit **3V3** / **5V** / **GND** headers to mic and amp per their ratings |

---

## Wiring diagram (ASCII)

```
  I2S microphone                         ESP32-S3 DevKit              I2S speaker amp
  (e.g. INMP441)                         (your board)                 (e.g. MAX98357A)

        VDD ───────────────────────────── 3V3
        GND ───────────────────────────── GND
        WS  ───────────────────────────── GPIO15  (I2S_WS)
        SCK ───────────────────────────── GPIO16  (I2S_SCK)
        SD  ───────────────────────────── GPIO4   (I2S_SD)


                                        GPIO17  (I2S_OUT_WS) ───────── LRC / WS
                                        GPIO18  (I2S_OUT_SCK) ──────── BCLK
                                        GPIO21  (I2S_OUT_SD) ────────── DIN
                                        3V3 or 5V (per module) ─────── VIN/VDD
                                        GND ────────────────────────── GND


  [Optional — only if OPEN_EVE_HARDWARE_WAKE_GPIO is set to a valid GPIO in code]
                                        GPIO <N> ◄──────┐
                                        GND ◄──────────┘  (momentary switch; pull-up in firmware)
```

---

## ESP32-S3-DevKitC-1 header positions (v1.1)

If your board is [ESP32-S3-DevKitC-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html#header-block), **No.** on **J1** / **J3** matches Espressif’s table:

| GPIO | Header |
| ---- | ------ |
| 4 | J1 — 4 |
| 15 | J1 — 8 |
| 16 | J1 — 9 |
| 17 | J1 — 10 |
| 18 | J1 — 11 |
| 21 | J3 — 18 |

---

## Not wired in `speak.ino`

- **Wi‑Fi / HTTPS** — on-chip, no extra pins.
- **Bluetooth audio** — not used.
- **Any GPIO not listed above** — unused unless you add code or set `OPEN_EVE_HARDWARE_WAKE_GPIO`.

---

## Configure, build, layout

- Edit Wi‑Fi, `BACKEND_HOST`, wake rules, and buffers at the top of [`speak.ino`](./speak.ino).
- PlatformIO (repo root): `pio run -e esp32-s3-speak -t upload` then `pio device monitor -e esp32-s3-speak -b 115200`.

```
open-eve/
└── src/speak/
    ├── speak.ino
    └── README.md
```
