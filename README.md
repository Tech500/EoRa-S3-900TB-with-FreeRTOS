# EoRa-S3-900TB with FreeRTOS — LoRa Camera Control System --Avg 174 µA

A long-range wireless camera battery control system built on the **EoRa-S3-900TB** development board (ESP32-S3 + SX1262 LoRa). A FreeRTOS-based Transmitter (TX) sends commands over LoRa to a deep-sleep Receiver (RX) that controls camera power via a KY-002S bi-stable latching switch.

---

## Hardware

| Component | Description |
|---|---|
| ** 2 - Ebyte, EoRa-S3-900TB** | ESP32-S3 development board with onboard SX1262 LoRa (900 MHz) |
| ** Avaiable from: ** [EbyteIoT.com](https://ebyteiot.com/products/ebyte-oem-odm-eora-s3-900tb-22dbm-7km-mini-low-power-and-long-distance-sx1262-rf-module-lora-module-915mhz) | Must meet local ISM band frequency. |
| **KY-002S** Switch| Bi-stable latching switch — holds state without power, triggered by HIGH→LOW pulse |

### Pin Definitions (RX)

| Pin | Function |
|---|---|
| `KY002S_TRIGGER` | GPIO — HIGH→LOW pulse to toggle switch latch |
| `KY002S_STATUS` | Input only — reads actual switch state on boot |
| `GPIO_NUM_16` | EXT0 wakeup — LoRa DIO1 interrupt |

---

## Architecture Overview

```
TX (FreeRTOS / AsyncWebServer)        RX (Deep Sleep)
──────────────────────────────        ───────────────
Web request → /relay route
  → command = 1, sendRequested = true
  → txTask picks up
  → sends WOR preamble (0xAA) ──LoRa──→  EXT0 wakes RX
  → sends command packet (0xBB) ──────→  RX receives cmd
                                              │
                                         Web link → camera ON (KY-002S pulse)
                                              → radio.sleep()
                                              → Timer sleep (120s)
                                              │
                                         Timer wakes RX
                                              → camera OFF (KY-002S pulse)
                                              → loraReset()
                                              → EXT0 sleep
                                         
```

---

## TX — Transmitter (FreeRTOS) 

TX only used for one preamble and command packet; conserving battery power.

The TX runs on **Core 1** with an `AsyncWebServer` handling web requests and a dedicated `txTask` processing LoRa sends.

### Tasks

**`txTask` (Core 1, priority 1)**
Waits for `sendRequested` flag set by web route handler, then transmits the LoRa command packet. Blocks on `ntpSynced` flag before processing any time-stamped messages.

**`ntpTask` (Core 0, priority 2)**
Syncs NTP time on boot using `configTzTime()`. Higher priority than `txTask` to guarantee time is valid before any packet timestamp is generated. Self-deletes after successful sync.

### Web Routes

| Route | Method | Action |
|---|---|---|
| `/relay` | GET | Serves HTML control page, sets `command = 1`, `sendRequested = true` |

### LoRa Packet Types

| Type | Value | Purpose |
|---|---|---|
| Wake packet | `0xAA` | WOR preamble — wakes RX from deep sleep |
| Command packet | `0xBB` | Carries `cmd` (1) and `timestr` timestamp |
| ACK packet | `0xFF` | Acknowledgement from RX back to TX |

---

## RX — Receiver (Deep Sleep)

The RX uses **ESP32 deep sleep** exclusively — no FreeRTOS, no `Ticker`. All timing is handled via `esp_sleep_enable_timer_wakeup()` and `RTC_DATA_ATTR` variables that survive deep sleep.

### Wakeup Sources

| Wakeup | Source | Action |
|---|---|---|
| **EXT0** | LoRa DIO1 / GPIO16 rising edge | Receive LoRa packet, process command |
| **Timer** | 120 second countdown | Cut camera power, reset radio, return to EXT0 sleep |

### Deep Sleep Modes

**SLEEP-1 — EXT0 (LoRa Listen)**
Radio placed in `startReceiveDutyCycleAuto()` WOR mode before sleep. DIO1 fires on preamble detection, EXT0 wakes the ESP32.

**SLEEP-2 — Timer (Camera Window)**
Entered after `cmd 1` only. Radio placed in `radio.sleep()`. Camera remains ON for 120 seconds. Timer wakeup cuts camera power and resets radio.

### Command Handling

| Command | Action |
|---|---|
| `cmd 1` | Camera ON (KY-002S pulse) → `radio.sleep()` LoRa module powered down → Timer sleep (120s) |
| `Timer Expired` | Camera OFF immediately (KY-002S pulse) → EXT0 sleep |

### KY-002S Switch Control

The KY-002S is a **bi-stable latching switch** — it holds its state indefinitely without power. A single HIGH→LOW pulse on `KY002S_TRIGGER` toggles the switch state.

On **cold boot**, `initRelayState()` reads the `KY002S_STATUS` pin to determine the true switch state and pulses OFF if the camera is found ON — no blind assumptions.

```
Cold boot
  └─→ readRelayStatus()       read KY002S_STATUS pin
        ├── HIGH (camera ON)  → pulseCameraRelay() → camera OFF
        └── LOW (camera OFF)  → no pulse needed
  └─→ enterDeepSleepEXT0()
```

### RTC Persistent Variables

| Variable | Purpose |
|---|---|
| `cameraIsOn` | Tracks switch state across deep sleep cycles |
| `cameraTimerArmed` | Guards timer wakeup — only acts if `cmd 1` was received |
| `lastKnownTime` | Last received timestamp string from TX |
| `lastCommand` | Last received command number |

> **Note:** `cameraTimerArmed` is the gate against stale timer wakeups. Since `esp_sleep_enable_timer_wakeup()` persists across deep sleep cycles and cannot be reliably disabled, the flag ensures the timer wakeup handler only cuts camera power when legitimately armed by `cmd 1`.

### Radio Reset on Timer Wakeup

After `radio.sleep()` + ESP32 deep sleep, the SX1262 may be in an unknown state on wakeup. `loraReset()` performs a hardware RST pin pulse followed by `initRadio()` re-initialization before returning to EXT0 sleep.

```cpp
// radio.reset() — called on timer wakeup only
RST pin LOW (10ms) → HIGH (10ms) → initRadio()

// initRadio() — called on cold boot (chip already clean)
radio.setPins() → radio.begin()
```

---

## Project Structure

```
EoRa-S3-900TB-with-FreeRTOS/
├── EoRA-S3-900TB_FreeRTOS_Transmitter/
│   ├── main.cpp          # setup(), FreeRTOS tasks, web routes
│   ├── radio_eora.h/cpp  # SX1262 init, WOR, transmit helpers
│   ├── packet_struct.h   # LoraPacket, AckPacket structs
│   ├── utilities.h       # Pin definitions, NTP, timestamp helpers
│   └── boards.h          # EoRa-S3-900TB peripherals setup
│
├── EoRA-S3-900TB_FreeRTOS_Receiver/
│   ├── main.cpp          # setup(), deep sleep state machine
│   ├── radio_eora.h/cpp  # SX1262 init, WOR, receive helpers
│   ├── packet_struct.h   # LoraPacket, AckPacket structs
│   ├── utilities.h       # Pin definitions, NTP, timestamp helpers
│   └── boards.h          # EoRa-S3-900TB peripherals setup
│
└── LICENSE               # MIT
```

---

## Dependencies

| Library | Purpose |
|---|---|
| [RadioLib](https://github.com/jgromes/RadioLib) | SX1262 LoRa driver, WOR duty cycle |
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | Async web server on TX |
| [AsyncTCP](https://github.com/me-no-dev/AsyncTCP) | Required by ESPAsyncWebServer |
| Arduino ESP32 core | FreeRTOS, deep sleep, RTC GPIO APIs |

---

## Build Environment

- **Platform:** ESP32-S3
- **Framework:** Arduino (with FreeRTOS)
- **IDE:** PlatformIO or Arduino IDE 2.x
- **Board:** EoRa-S3-900TB (select appropriate ESP32-S3 board definition)
- **Frequency:** 915 MHz (US) — adjust in `radio_eora` init for your region

---

## Deep Sleep Flow Summary

```
Cold Boot
  └─→ initRelayState()  →  enterDeepSleepEXT0()

EXT0 Wake (LoRa WOR preamble detected)
  └─→ initRadio()  →  startReceive()
        ├── 0xAA (WOR wake packet)  →  startReceive() continue
        └── 0xBB (command packet)   →  sendAck()  →  handleCommand()
                ├── cmd 1  →  pulseCameraRelay (ON)  →  radio.sleep()  →  Timer wake up set → enterDeepSleepEXT0 
                └── Timer expired  →  pulseCameraRelay (OFF) →  enterDeepSleepEXT0()

Timer Wake (120s camera window expired)
  └─→ cameraTimerArmed?
        ├── YES  →  pulseCameraRelay (OFF)  →  loraReset()  →  enterDeepSleepEXT0()
        └── NO   →  stale timer, ignored   →  enterDeepSleepEXT0()
```
##  Credits & Acknowledgements

This project was developed with testing and guidance from:
- **William Lucid** – Founder & Developer  
- **OpenAI ChatGPT** – Engineering Assistant & Debugging Partner
- **Claude** – Lead programmer & Debugger, Battery Analysis, "EoRa_PI_WOR_Receiver.ino"  
- **Copilot**, "DIO1 re-routing", and Contributions to coding
- **Gemini** – Support, Debugging, and Contributions to coding
  
- Community testers and contributors
---

## License

MIT License — Copyright (c) 2026 William Lucid, AB9NQ (Tech500)

See [LICENSE](LICENSE) for full terms.

---

*73 de AB9NQ (Tech500)*
