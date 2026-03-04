#define EoRa_PI_V1
#include <Arduino.h>
#include <RadioLib.h>
#include "boards.h"
#include "packet_struct.h"
#include "utilities.h"
#include "radio_eora.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

RTC_DATA_ATTR bool cameraIsOn = false;
RTC_DATA_ATTR bool cameraTimerArmed = false;
RTC_DATA_ATTR char lastKnownTime[48] = "";
RTC_DATA_ATTR int lastCommand = 0;

#define TIME_TO_SLEEP 120  // Sleep for 120 seconds

// -------------------- Flags --------------------
extern SX1262 radio;
extern volatile bool enableInterrupt;
extern volatile bool receivedFlag;  // already defined in your ISR file
extern void setFlag();              // ISR from radio_eora

uint8_t command = 0;

volatile bool packetDone = false;

void packetHandler() {
  packetDone = true;
}

volatile bool ackReceived = false;

void IRAM_ATTR onDio1() {
  ackReceived = true;
}

// -------------------- Forward Declarations --------------------
void handleWorWake();
void handleTimerWake();
void enterDeepSleepEXT0();

void handlePacket(const LoraPacket& pkt) {
  if (pkt.type != 0xBB) {
    Serial.printf("RX: Ignoring non-command packet (type=0x%02X)\n", pkt.type);
    return;  // ← drop WOR preamble and anything else unexpected
  }

  Serial.printf("RX: CMD = %u\n", pkt.cmd);
  Serial.printf("RX: Timestamp = %s\n", pkt.timestr);

  sendAck();
}

void sendAck() {
  AckPacket ack;
  ack.type = 0xFF;
  ack.status = 1;

  radio.setPreambleLength(8);
  radio.transmit((uint8_t*)&ack, sizeof(ack));

  radio.startReceiveDutyCycleAuto();

  Serial.println("RX: ACK sent");
}

// ───────────────────────────────────────────────────────────────────
//  KY-002S Status & Relay
// ───────────────────────────────────────────────────────────────────
bool readRelayStatus() {
  pinMode(KY002S_STATUS, INPUT);
  bool state = digitalRead(KY002S_STATUS);
  Serial.printf("[STATUS] KY-002S: %s\n", state ? "HIGH (ON)" : "LOW (OFF)");
  return state;
}

void pulseCameraRelay() {
  pinMode(KY002S_TRIGGER, OUTPUT);
  digitalWrite(KY002S_TRIGGER, HIGH);
  delay(PULSE_MS);
  digitalWrite(KY002S_TRIGGER, LOW);  // falling edge → latch triggers

  cameraIsOn = !cameraIsOn;
  Serial.printf("[RELAY] Pulsed → Camera: %s\n", cameraIsOn ? "ON" : "OFF");
}

// ───────────────────────────────────────────────────────────────────
//  Cold boot init — read status pin, ensure camera OFF
// ───────────────────────────────────────────────────────────────────
void initRelayState() {
  cameraIsOn = readRelayStatus();
  if (cameraIsOn) {
    Serial.println("[INIT] Camera ON at boot — pulsing OFF");
    pulseCameraRelay();
  } else {
    Serial.println("[INIT] Camera already OFF");
  }
  cameraTimerArmed = false;
  lastCommand = 0;
}

// ---------------------------
// SETUP
// ---------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_16);

  setCpuFrequencyMhz(80);

  initRelayState();

  initBoard();
  delay(1500);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wake cause: %d\n", cause);

  // ================================================================
  // TIMER WAKEUP — 120s camera window expired
  // ================================================================
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke from timer — cutting camera power");

    if (cameraTimerArmed) {
      initRelayState();  // Read KY-002S status pin

      if (cameraIsOn) {
        pulseCameraRelay();  // Cut camera
      }

      cameraTimerArmed = false;
    }

    radio.reset();         // Hardware reset + re-init after LoRa.sleep()
    enterDeepSleepEXT0();  // Back to listening for next command
    return;
  }

  // ================================================================
  // COLD BOOT / POWER ON — ensure camera OFF, enter EXT0 sleep
  // ================================================================
   if (cause != ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("Cold boot → camera init → deep sleep");
        initRelayState();  // Read KY-002S status pin
        enterDeepSleepEXT0();
        return;
    }
  // ================================================================
  // EXT0 WAKEUP — LoRa packet received
  // ================================================================
  Serial.println("Woke from EXT0 — LoRa packet ready");

  initRelayState();
  pulseCameraRelay();

  initRadio();
  radio.setDio1Action(packetHandler);
  radio.startReceive();

  bool gotPacket = false;
  uint32_t start = millis();
  uint8_t rxBuf[sizeof(LoraPacket)];
  LoraPacket msg;

  while (millis() - start < 5000) {
    if (packetDone) {
      packetDone = false;
      int state = radio.readData(rxBuf, sizeof(LoraPacket));
      Serial.printf("RX: readData state=%d\n", state);

      if (state == RADIOLIB_ERR_NONE) {
        memcpy(&msg, rxBuf, sizeof(LoraPacket));
        Serial.printf("RX: cmd=%u time=%s\n", msg.cmd, msg.timestr);

        if (msg.type == 0xAA) {
          Serial.println("RX: Wake packet — waiting for command...");
          radio.startReceive();
          continue;
        }

        if (msg.type == 0xBB) {
          sendAck();
          delay(500);
          strncpy(lastKnownTime, msg.timestr, sizeof(lastKnownTime));
          handleCommand(msg.cmd, msg.timestr);
          gotPacket = true;
          break;
        }
      }
    }
    delay(10);
  }

  if (!gotPacket) {
    Serial.println("RX: No valid packet — back to sleep");
    enterDeepSleepEXT0();
  }
}

void loop() {
  delay(1000);  // should never run
}

// ───────────────────────────────────────────────────────────────────
//  Handle command — no Ticker, sleep type decided here
// ───────────────────────────────────────────────────────────────────
void handleCommand(uint8_t command, const char* timeStr) {
  Serial.printf("RX: Command %u at %s\n", command, timeStr);

  if (command == 1) {
    Serial.println("RX: cmd 1 — camera ON, arming 120s timer sleep");

    if (!cameraIsOn) {
      pulseCameraRelay();  // Camera ON
    }

    cameraTimerArmed = true;

    radio.sleep();          // LoRa sleep before deep sleep
    enterDeepSleepTimer();  // ← Timer sleep, NOT EXT0

  } else if (command == 2) {
    Serial.println("RX: cmd 2 — camera OFF, back to EXT0 sleep");

    if (cameraIsOn) {
      pulseCameraRelay();  // Camera OFF immediately
    }

    cameraTimerArmed = false;
    enterDeepSleepEXT0();  // ← Back to listening

  } else {
    Serial.println("RX: Unknown command — back to EXT0 sleep");
    enterDeepSleepEXT0();
  }
}

void enterDeepSleepEXT0() {
  Serial.println("[SLEEP-1] EXT0 sleep — waiting for LoRa WOR...");
  Serial.flush();

  // ── Radio must be in WOR duty cycle before EXT0 sleep ─────────
  initRadio();
  radio.setDio1Action(packetHandler);
  radio.startReceiveDutyCycleAuto();  // ← WOR mode — DIO1 fires on preamble

  // ── Clear any pending timer wakeup ────────────────────────────
  //esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  // ── EXT0 on DIO1 GPIO16 rising edge ───────────────────────────
  rtc_gpio_pulldown_en(GPIO_NUM_16);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 1);

  digitalWrite(BOARD_LED, LED_OFF);
  SPI.end();

  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(GPIO_NUM_16);

  esp_deep_sleep_start();
}

// ───────────────────────────────────────────────────────────────────
//  SLEEP 2 — Timer only (camera window 120s)
// ───────────────────────────────────────────────────────────────────
void enterDeepSleepTimer() {
  Serial.printf("[SLEEP-2] Timer sleep — camera OFF in %d sec...\n", TIME_TO_SLEEP);
  Serial.flush();

  // ── Disable EXT0 — no LoRa wake during camera window ─────────
  //esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  esp_sleep_enable_timer_wakeup((uint64_t)TIME_TO_SLEEP * 1000000ULL);

  esp_deep_sleep_start();
}

// -------------------- WOR Wake Handler --------------------
void handleWorWake() {
  Serial.println("=== WOR WAKE ===");

  // Timestamp of wake
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  char buffer[32];
  strftime(buffer, 32, "%Y-%m-%d %H:%M:%S", tm_info);
  Serial.printf("Local time: %s\n", buffer);

  Serial.println("Waiting for packet...");

  uint32_t start = millis();
  while (!receivedFlag && (millis() - start) < 5000) {
    delay(1);
  }

  if (!receivedFlag) {
    Serial.println("No packet after WOR wake. Re-arming WOR.");
    initRadio();
    enterDeepSleepEXT0();
  }

  receivedFlag = false;

  LoraPacket pkt;
  int state = radio.readData((uint8_t*)&pkt, sizeof(pkt));

  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("readData failed: ");
    Serial.println(state);
    initRadio();
    enterDeepSleepEXT0();
  }

  Serial.println("Packet received:");
  Serial.printf("  type: %u\n", pkt.type);
  Serial.printf("  cmd: %u\n", pkt.cmd);
  Serial.printf("  timestamp: %u\n", pkt.timestr);

  Serial.printf("  TX time: %s\n", pkt.timestr);

  // -------------------- CMD 1: Camera ON --------------------
  if (pkt.cmd == 1) {
    Serial.println("Turning camera ON...");
    pulseCameraRelay();

    Serial.println("Putting radio to sleep...");
    radio.sleep();  // eliminates 8mA WOR spikes

    const uint64_t ON_DURATION_US = 2ULL * 60ULL * 1000000ULL;  // 2 minutes
    Serial.println("Starting deep sleep timer for camera ON duration...");

    esp_sleep_enable_timer_wakeup(ON_DURATION_US);

    Serial.println("=== ENTERING TIMER SLEEP ===");
    delay(50);  // allow Serial flush
    esp_deep_sleep_start();
  }

  // -------------------- Unknown command --------------------
  Serial.println("Unknown cmd. Re-arming WOR.");
  initRadio();
  enterDeepSleepEXT0();
}

// -------------------- Timer Wake Handler --------------------
void handleTimerWake() {
  Serial.println("=== TIMER WAKE ===");

  // Print local timestamp of wake
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  char buffer[32];
  strftime(buffer, 32, "%Y-%m-%d %H:%M:%S", tm_info);
  Serial.printf("Local time: %s\n", buffer);

  // Camera OFF event
  Serial.println("Turning camera OFF...");
  pulseCameraRelay();

  // Radio re-init
  Serial.println("Re-initializing radio...");
  initRadio();

  // Re-arm WOR
  Serial.println("Re-entering WOR mode...");
  radio.setDio1Action(setFlag);
  radio.startReceiveDutyCycleAuto();

  // Prepare for deep sleep
  Serial.println("Preparing for deep sleep (WOR)...");
  esp_sleep_enable_ext0_wakeup((gpio_num_t)GPIO_NUM_16, 1);

  Serial.println("=== ENTERING WOR SLEEP ===");
  delay(50);  // allow Serial to flush
  esp_deep_sleep_start();
}
