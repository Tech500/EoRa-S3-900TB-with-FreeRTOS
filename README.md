# EoRa-S3-900TB with FreeRTOS — LoRa Camera Control System — Avg 174 µA

A long-range wireless camera battery control system built on the **EoRa-S3-900TB** development board (ESP32-S3 + SX1262 LoRa). A FreeRTOS-based Transmitter (TX) sends commands over LoRa to a deep-sleep Receiver (RX) that controls camera power via a KY002S bi-stable latching switch.

---

## Background

Before moving to our apartment, there was a wetland pond behind our backyard. A Wyze Cam 3 was mounted above the shed to observe [Wildlife Activity](https://drive.google.com/file/d/1jVMsczDnD7FmhWtkw6Mw46ZPIvohf_RL/view?usp=sharing). The camera required a 10,000 mAh power bank, which barely lasted ~24 hours of continuous use before needing a swap.

The goal became powering the camera **only on demand** — when a viewer clicks the ["videofeed"](https://weather-observations.live/Videofeed.html) link on the hosted website. Clicking the **Observations II** link returns the viewer to the main menu.

---

## Hardware

| Component | Description |
| --- | --- |
| **2 × Ebyte EoRa-S3-900TB** | ESP32-S3 development board with onboard SX1262 LoRa (900 MHz) |
| **Available from:** [EbyteIoT.com](https://ebyteiot.com/products/ebyte-oem-odm-eora-s3-900tb-22dbm-7km-mini-low-power-and-long-distance-sx1262-rf-module-lora-module-915mhz) | Regulated; match local ISM band frequency |
| **KY002S** | Bi-stable latching switch — holds state without power, triggered by HIGH→LOW pulse |

### Pin Definitions (RX)

| Pin | Function |
| --- | --- |
| `KY002S_TRIGGER` | GPIO — HIGH→LOW pulse to toggle switch latch |
| `KY002S_STATUS` | Input only — reads actual switch state on boot |
| `GPIO_NUM_16` | EXT0 wakeup — LoRa DIO1 interrupt |

### Important Hardware Note

DIO1 is tied to **GPIO33**, which is **not** an RTC-capable wakeup pin. A direct wire from **DIO1 to GPIO_NUM_16** is all that is needed — no additional components required.

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

TX is only used for one preamble and command packet.

The TX runs on **Core 1** with an `AsyncWebServer` handling web requests and a dedicated `txTask` processing LoRa sends.

### Tasks

**`txTask` (Core 1, priority 1)**
Waits for `sendRequested` flag set by web route handler, then transmits the LoRa command packet. Blocks on `ntpSynced` flag before processing any time-stamped messages.

**`ntpTask` (Core 0, priority 2)**
Syncs NTP time on boot using `configTzTime()`. Higher priority than `txTask` to guarantee time is valid before any packet timestamp is generated. Self-deletes after successful sync.

### Web Routes

| Route | Method | Action |
| --- | --- | --- |
| `/relay` | GET | Serves HTML control page, sets `command = 1`, `sendRequested = true` |

### LoRa Packet Types

| Type | Value | Purpose |
| --- | --- | --- |
| Wake packet | `0xAA` | WOR preamble — wakes RX from deep sleep |
| Command packet | `0xBB` | Carries `cmd` (1) and `timestr` timestamp |
| ACK packet | `0xFF` | Acknowledgement from RX back to TX |

---

## RX — Receiver (Deep Sleep)

The RX uses **ESP32 deep sleep** exclusively — no FreeRTOS, no `Ticker`. All timing is handled via `esp_sleep_enable_timer_wakeup()` and `RTC_DATA_ATTR` variables that survive deep sleep.

### Power Profile *(measured with Nordic PPK2)*

| State | Current |
| --- | --- |
| `autoDutyCycle` listening spike (~9 ms active) | ~11 mA |
| Between spikes (`deep sleep`) | ~25.38 µA |
| Duty-cycle average | **~174 µA** |
| Between camera ON and camera OFF (`radio.sleep()`) | -17 - ~18 µA |

`autoDutyCycle` (no parameters) is responsible for the ~174 µA average current. When listening for 9 ms it consumes ~11 mA; current between spikes is ~25.38 µA.

📊 [NPPK II Observations (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/NPPK%20II%20Observations.pdf)
📊 [NPPK II Battery Life Analysis (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/NPPK_II_Battery_Life_Analysis.pdf)

### Wakeup Sources

| Wakeup | Source | Action |
| --- | --- | --- |
| **EXT0** | LoRa DIO1 / GPIO16 rising edge | Receive LoRa packet, process command |
| **Timer** | 120 second countdown | Cut camera power, reset radio, return to EXT0 sleep |

### Deep Sleep Modes

**SLEEP-1 — EXT0 (LoRa Listen)**
Radio placed in `startReceiveDutyCycleAuto()` WOR mode before sleep. DIO1 fires on preamble detection, EXT0 wakes the ESP32.

**SLEEP-2 — Timer (Camera Window)**
Entered after `cmd 1` only. Radio placed in `radio.sleep()`. Camera remains ON for 120 seconds. Timer wakeup cuts camera power and resets radio.  Camera is powered by a separate battery, power bank; from the LiPo battery.

### Command Handling

| Command | Action |
| --- | --- |
| `cmd 1` | Camera ON (KY002S pulse) → `radio.sleep()` → Timer sleep (120s) |
| `Timer Expired` | Camera OFF (KY002S pulse) → EXT0 sleep |

### KY002S Switch Control

The KY002S is a **bi-stable latching switch** — it holds its state indefinitely without power. A single HIGH→LOW pulse on `KY002S_TRIGGER` toggles the switch state.

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
| --- | --- |
| `cameraIsOn` | Tracks switch state across deep sleep cycles |
| `cameraTimerArmed` | Guards timer wakeup — only acts if `cmd 1` was received |
| `lastKnownTime` | Last received timestamp string from TX |
| `lastCommand` | Last received command number |

> **Note:** `cameraTimerArmed` is the gate against stale timer wakeups. Since `esp_sleep_enable_timer_wakeup()` persists across deep sleep cycles and cannot be reliably disabled, the flag ensures the timer wakeup handler only cuts camera power when legitimately armed by `cmd 1`.

### Radio Reset on Timer Wakeup

After `radio.sleep()` + ESP32 deep sleep, the SX1262 may be in an unknown state on wakeup. `radio.reset()` performs a hardware RST pin pulse followed by `initRadio()` re-initialization before returning to EXT0 sleep.

```
// radio.reset() — called on timer wakeup only
RST pin LOW (10ms) → HIGH (10ms) → initRadio()

// initRadio() — called on cold boot (chip already clean)
radio.setPins() → radio.begin()
```

### Deep Sleep Flow Summary

```
Cold Boot
  └─→ initRelayState()  →  enterDeepSleepEXT0()

EXT0 Wake (LoRa WOR preamble detected)
  └─→ initRadio()  →  startReceive()
        ├── 0xAA (WOR wake packet)  →  startReceive() continue
        └── 0xBB (command packet)   →  sendAck()  →  handleCommand()
                ├── cmd 1  →  pulseCameraRelay (ON)  →  radio.sleep()  →  Timer set  →  enterDeepSleepEXT0()
                └── (other) →  enterDeepSleepEXT0()

Timer Wake (120s camera window expired)
  └─→ cameraTimerArmed?
        ├── YES  →  pulseCameraRelay (OFF)  →  loraReset()  →  enterDeepSleepEXT0()
        └── NO   →  stale timer, ignored    →  enterDeepSleepEXT0()
```

---

## Project Structure

```
EoRa-S3-900TB-with-FreeRTOS/
├── EoRA-S3-900TB_FreeRTOS_Transmitter/
│   ├── EoRA-S3-900TB_FreeRTOS_Transmitter.ino
│   ├── boards.h          # EoRa-S3-900TB peripherals setup
│   ├── utilities.h       # Pin definitions, NTP, timestamp helpers
│   ├── packet_struct.h   # LoraPacket, AckPacket structs
│   ├── radio_eora.cpp    # SX1262 init, WOR, transmit helpers
│   └── radio_eora.h      # Radio function declarations and constants
│
├── EoRA-S3-900TB_FreeRTOS_Receiver/
│   ├── EoRA-S3-900TB_FreeRTOS_Receiver.ino
│   ├── boards.h          # EoRa-S3-900TB peripherals setup
│   ├── utilities.h       # Pin definitions, NTP, timestamp helpers
│   ├── packet_struct.h   # LoraPacket, AckPacket structs
│   ├── radio_eora.cpp    # SX1262 init, WOR, receive helpers
│   └── radio_eora.h      # Radio function declarations and constants
│
├── Doc/                  # Documentation and reference files
├──Images                 # Related images
├── Schematic             # Transmitter and Receiver Schematics
└── LICENSE               # MIT License

```

> **Important:** All files must reside in the **same sketch folder** as the `.ino` file. Arduino IDE requires all supporting files to be co-located with the main sketch for compilation to succeed.

---

## Project Video

<a href="https://player.vimeo.com/video/1170668110?autoplay=1" target="_blank">Demo of "EoRa-S3-900TB with FreeRTOS"</a>
Press 'F' key for full screen.

---

## Getting Started

### Before Compiling — Put EoRa-S3-900TB into Download Mode

The ESP32-S3 must be in **bootloader/download mode** before the Arduino IDE can flash it:

1. Connect the board to your computer via USB-C.
2. **Press and hold** the `BOOT` button on the board.
3. While holding `BOOT`, **press and release** the `RST` (Reset) button.
4. Release the `BOOT` button.
5. The board is now in download mode and ready to receive a sketch upload.
6. After flashing completes, press `RST` once to reboot into normal operation.

> **Note (Windows):** The COM port may change when entering download mode. Check Device Manager if the expected port has disappeared.

### Cold Boot to Sync

After flashing both the **transmitter** and **receiver**, perform a **cold boot** on both units to synchronize them. A cold boot is required after every fresh flash.

Open browser; type the server ipAddress/relay in the address bar, press enter to trigger KY002S switch, refresh browser.  Transmitter uses a static ipAddress 192.168.12.27; will likely need to be edited, find it in wifi_Start function:  edit ipAddress, gateway, and dns.

---

## Arduino IDE Setup

### Step 1 — Install Arduino IDE

Download and install **Arduino IDE 2.x** from [arduino.cc/en/software](https://www.arduino.cc/en/software).

---

### Step 2 — Add ESP32 Board Manager URL

1. Go to **File → Preferences**
2. In the **"Additional Board Manager URLs"** field, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**

---

### Step 3 — Install ESP32 Board Package

1. Go to **Tools → Board → Boards Manager**
2. Search for **`esp32`**
3. Install **"esp32 by Espressif Systems"** (latest version)
4. Click **Close**

---

### Step 4 — Install Required Libraries

1. Go to **Tools → Manage Libraries**
2. Search for and install each of the following:

| Library | Author |
| --- | --- |
| **RadioLib** | Jan Gromes |
| **ESPAsyncWebServer** | me-no-dev |
| **AsyncTCP** | me-no-dev |

---

### Step 5 — Board and Tools Menu Configuration

1. Go to **Tools → Board → ESP32 Arduino** and select **`ESP32S3 Dev Module`**
2. Configure the Tools menu as follows:

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Port | Your COM port (Windows) or `/dev/tty...` (macOS/Linux) |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Core Debug Level | None |
| USB DFU On Boot | Disabled |
| Erase All Flash Before Sketch Upload | Disabled |
| Flash Mode | **QIO 80MHz** |
| Flash Size | **4MB (32Mb)** |
| Partition Scheme | Default 4MB with spiffs |
| PSRAM | **OPI PSRAM** |
| Upload Mode | **UART0 / Hardware CDC** |
| Upload Speed | 921600 |

> **PSRAM:** Must be set to **OPI PSRAM** to match the ESP32-S3FH4R2's 2MB OPI PSRAM.

> **USB CDC On Boot:** Set to **Enabled** when using the USB-C port for Serial Monitor output.

> ⚠️ **Frequency:** 915 MHz (US) — adjust `radio_eora` init for your region. ISM band is regulated by region and must match the frequency the board was ordered for.

---

### Step 6 — Open and Upload the Sketch

1. Open the **Receiver** or **Transmitter** sketch folder in Arduino IDE.
2. Confirm all supporting files (`boards.h`, `utilities.h`, `packet_struct.h`, `radio_eora.cpp`, `radio_eora.h`) are present alongside the `.ino` file.
3. Put the EoRa-S3-900TB into **download mode** (see above).
4. Click the **Upload** button (→) in the Arduino IDE toolbar.
5. After flashing completes, press **RST** to reboot.
6. Repeat for the second unit (transmitter or receiver).
7. Perform a **cold boot** on both units to sync.

---

## Dependencies

| Library | Purpose |
| --- | --- |
| [RadioLib](https://github.com/jgromes/RadioLib) | SX1262 LoRa driver, WOR duty cycle |
| [ESPAsyncWebServer]([ESP32Async](https://github.com/ESP32Async/ESPAsyncWebServer)) | Async web server on TX |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP)) | Required by ESPAsyncWebServer |
| Arduino ESP32 core | FreeRTOS, deep sleep, RTC GPIO APIs |

---

## Documentation & References

### Hardware Reference
📄 [Complete Ebyte EoRa-S3-900TB (EoRa PI) Pin Mapping Guide (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/Complete%20Ebyte%20EoRa-S3-900TB%20(EoRa%20PI)%20Pin%20Mapping%20Guide.pdf)
📄 [KY002S MOSFET Switch Module Specification (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/KY002S%20MOSFET%20Switch%20Module%20Specification.pdf)

### Pin Definitions
🖼️ [EoRa-S3-900TB Pin Definition](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/EoRa-S3-900TB%20Pin%20Definition.png)
🖼️ [Pin Definition](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/Pin%20Definition.png)

### Project Photos
🖼️ [EoRa-S3-900TB with FreeRTOS](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/EoRa-S3-900TB%20with%20FreeRTOS.png)
🖼️ [Homelab Setup](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/EoRa-S3-900TB--with-FreeRTOS--homelab.png)

### Power Analysis
📊 [NPPK II Observations (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/NPPK%20II%20Observations.pdf)
📊 [NPPK II Battery Life Analysis (PDF)](https://github.com/Tech500/EoRa-S3-900TB-with-FreeRTOS/blob/main/Doc/NPPK_II_Battery_Life_Analysis.pdf)

---

## Credits & Acknowledgements

This project was developed with testing and guidance from:

* **William Lucid** – Founder & Developer
* **OpenAI ChatGPT** – Engineering Assistant & Debugging Partner
* **Claude** – Lead Programmer & Debugger, Battery Analysis, `EoRa_PI_WOR_Receiver.ino`
* **Copilot** – DIO1 re-routing and contributions to coding
* **Gemini** – Support, debugging, and contributions to coding
* Community testers and contributors

---

## License

MIT License — Copyright (c) 2026 William Lucid

See [LICENSE](LICENSE) for full terms.

---

*73 de AB9NQ (Tech500)*
