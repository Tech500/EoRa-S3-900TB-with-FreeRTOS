# Why the EoRa-S3-900TB?

Choosing the right hardware for a battery-powered, always-listening receiver isn't
just about picking a board with LoRa on it. The wrong board kills your battery in
days. The right one survives for months — or longer. Here's why the Ebyte
EoRa-S3-900TB earned its place in this project.

---

## Everything in One Package

The EoRa-S3-900TB packs an ESP32-S3FH4R2 (dual-core LX7 @ 240 MHz, 4 MB Flash,
2 MB PSRAM, Wi-Fi, BLE 5.0) and an Ebyte E22-900MM22S LoRa module (SX1262, 22 dBm,
~7 km range) onto a single 27 × 64 mm board, along with a 0.96-inch OLED, SD card
slot, Type-C interface, and — critically — a built-in lithium battery charging
circuit. No separate modules to wire up. No charge IC to source. One board, one
LiPo connector, done.

---

## A Power Architecture Built for Battery Life

This is the detail most Ebyte reviews skip over. The EoRa-S3-900TB uses **two
separate LDO regulators** — LDOA and LDOB — feeding the ESP32 and the LoRa
peripherals on isolated 3.3 V rails:

| Rail | Regulator | Powers | Max Continuous | Peak Pulse |
|------|-----------|--------|----------------|------------|
| LDOA | HT7533 | ESP32-S3 | 250 mA → 3.3 V | 340 mA |
| LDOB | ME6214 | E22 LoRa + OLED + peripherals | 150 mA → 3.3 V | 100 mA |

Why does this matter? The ESP32's Wi-Fi and Bluetooth transceiver draws sudden
300+ mA spikes. On a single-LDO board, those transients collapse the supply rail
just long enough to corrupt a LoRa packet in flight, or worse, trigger a false
wakeup. The split-rail design keeps the SX1262 on clean, isolated power — exactly
what you need when a false wakeup means the camera switches on at 3 AM with nobody
watching.

---

## The OEM Spec vs. What You Actually Achieve

Ebyte's datasheet quotes a deep sleep floor of **~25 µA** — measured with USB
disconnected, OLED off, LoRa radio in full sleep, no peripherals active, running
on battery alone. That's the theoretical minimum: just the ESP32 RTC domain alive,
waiting for an external wakeup event.

Real-world with Wake-on-Radio running, deep sleep is ~174 µA. That is not a
separate mode — it *is* deep sleep for this project. The SX1262 autoDutyCycle
wakes for 9.2 ms every duty cycle interval to sniff for a preamble, then the
system returns to the ESP32 RTC baseline. Those two states time-average to 174 µA:

- **9.2 ms active sniff** @ ~11 mA — SX1262 listening
- **Remainder of duty cycle** @ ~25.38 µA — ESP32 RTC baseline

The 25.38 µA baseline, measured on the Nordic PPK2, validates the OEM ~25 µA spec
exactly. It is the floor beneath the duty cycle, not a separate operating mode.

| Mode | Current |
|------|---------|
| OEM deep sleep floor spec (no USB, peripherals off, battery) | ~25 µA |
| Deep sleep + WOR duty cycle — measured (Nordic PPK2) | ~174 µA |
| Active sniff spike (9.2 ms, SX1262) | ~11 mA |
| `radio.sleep()` — total system, 120 s viewing window (Nordic PPK2) | 18.39 µA avg / 19.55 µA max |
| High activity — TX / processing | 37–50 mA |

That 174 µA — achieved on a **development board** with an OLED, SD slot, and
battery charger on the PCB — is the headline. Bare modules can go lower. This
board, with everything integrated, holds it to 174 µA. That's the number that
makes 13–19 months of field deployment on a 3000 mAh LiPo realistic.

### One Complete Cycle — Power States in Sequence

Once a wake packet arrives and the camera switches on, the power profile shifts
entirely:

1. **Preamble detected** — EXT0 fires on GPIO_NUM_16, ESP32 wakes from deep sleep
2. **Receive + ACK** — ESP32 and SX1262 active, 37–50 mA
3. **Camera ON** — KY-002S latching switch pulsed, power bank energised
4. **`radio.sleep()` called** — total system current drops to a flat ~18.39 µA average (19.55 µA max, measured Nordic PPK2 ampere mode, in series with battery); SX1262 datasheet sleep spec is 600 nA–1.2 µA, so the ~18.39 µA is almost entirely the ESP32 RTC domain; ESP32 sets 120 s timer and holds in deep sleep for the full viewing window
5. **Timer fires** — ESP32 wakes, pulses switch to power camera OFF
6. **WOR resumes** — SX1262 returns to autoDutyCycle, system back to ~174 µA average

`radio.sleep()` only appears in step 4 — the quiet interval between camera ON and
camera OFF. It plays no part in the idle WOR listening phase.

---

## The One Hardware Gotcha

The SX1262's DIO1 interrupt line — the pin that fires when a preamble is detected
— is wired to GPIO33 on the EoRa-S3-900TB. GPIO33 is not RTC-capable on the
ESP32-S3, so it cannot serve as an EXT0 deep sleep wakeup source.

The fix: a single jumper wire from the DIO1 pad to GPIO_NUM_16, which *is*
RTC-capable. No extra ICs, no inverter buffer circuit. One wire, and the SX1262
can wake the ESP32 directly from deep sleep the moment a valid preamble arrives.

---

## Why Not a Bare Module?

A bare ESP32-S3 + separate SX1262 module would get you to a lower idle floor, but
you'd be designing your own LDO split, your own charge circuit, your own PCB
layout. The EoRa-S3-900TB ships with all of that already solved, at ~$18–20 USD,
in a form factor that fits in a weatherproof enclosure with room to spare. For a
field-deployed, battery-powered receiver with no mains power at the site, that
tradeoff is an easy one.
