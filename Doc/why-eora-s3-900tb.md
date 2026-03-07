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

Real-world with Wake-on-Radio running is a different story. The SX1262 duty cycle
mode wakes briefly every 9 ms to sniff the air for a preamble, then sleeps again.
That periodic listening adds current. Nordic PPK2 measurements on this exact board
confirm the operational modes:

| Mode | Current |
|------|---------|
| OEM deep sleep floor (no USB, peripherals off, battery) | ~25 µA |
| Radio Sleep between duty cycle spikes | ~25.38 µA |
| Deep Sleep with WOR duty cycle listening (this project) | ~174 µA average |
| Duty cycle active spike | ~11 mA (brief) |
| High activity — TX / processing | 37–50 mA |

The 174 µA average in this project is the *real* operating point: ESP32 in deep
sleep + SX1262 running autoDutyCycle at 9 ms intervals, listening for a LoRa
preamble 24/7. That 174 µA figure — achieved on a **development board** with an
OLED, SD slot, and battery charger on the PCB — is the headline. Bare modules can
go lower. This board, with everything integrated, holds it to 174 µA. That's the
number that makes 13–19 months of field deployment on a 3000 mAh LiPo realistic.

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
