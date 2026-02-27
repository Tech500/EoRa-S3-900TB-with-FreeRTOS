//new utilites.h

#pragma once
#include <Arduino.h>
#include "esp_sleep.h"

#define UNUSE_PIN                   (0)

#if defined(EoRa_PI_V1)

#define I2C_SDA                     18
#define I2C_SCL                     17
#define OLED_RST                    UNUSE_PIN

#define RADIO_SCLK_PIN              5
#define RADIO_MISO_PIN              3
#define RADIO_MOSI_PIN              6
#define RADIO_CS_PIN                7
#define RADIO_DIO1_PIN              33
#define RADIO_BUSY_PIN              34
#define RADIO_RST_PIN               8

// DIO1 -> 74HC04 -> 74HC04 -> GPIO16 (RTC)
#define WAKE_PIN                    GPIO_NUM_16

// -------------------- Camera Power Pins --------------------
#define KY002S_TRIGGER              45   // Camera ON/OFF control
#define KY002S_STATUS               44   // Status input 

#define SDCARD_MOSI                 11
#define SDCARD_MISO                 2
#define SDCARD_SCLK                 14
#define SDCARD_CS                   13

#define BOARD_LED                   37
#define LED_ON                      HIGH
#define LED_OFF                     LOW

#define BAT_ADC_PIN                 1
#define BUTTON_PIN                  0

#else
#error "For the first use, please define the board version and model in <utilities.h>"
#endif

// ---- Wake / SAFE-BOOT helpers ----

inline esp_sleep_wakeup_cause_t get_wakeup_cause() {
  return esp_sleep_get_wakeup_cause();
}

// Your rule: power-on reset = any wake that is NOT EXT0
inline bool is_ext0_wakeup() {
  return get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
}

inline bool is_power_on_reset() {
  return get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0;
}

inline void print_wakeup_reason() {
  Serial.print(F("[SAFE-BOOT] Wake reason: "));
  Serial.println((int)get_wakeup_cause());
}

inline void configure_ext0_wakeup() {
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, 1);  // wake on HIGH
}

inline void go_to_deep_sleep(uint64_t us = 0) {
  if (us > 0) {
    esp_sleep_enable_timer_wakeup(us);
  }
  configure_ext0_wakeup();
  Serial.println(F("[Sleep] Going to deep sleep..."));
  Serial.flush();
  esp_deep_sleep_start();
}


