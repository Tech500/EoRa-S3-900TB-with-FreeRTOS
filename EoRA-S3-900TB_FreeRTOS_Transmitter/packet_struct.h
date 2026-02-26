//packet_struct.h
#pragma once
#include <Arduino.h>

struct __attribute__((packed)) LoraPacket {
    uint8_t type;          // 1 = command packet
    uint8_t cmd;           // your command ID
    char timestr[48];      // human-readable timestamp with DOW
};

struct __attribute__((packed)) AckPacket {
    uint8_t type;      // always 0xFF
    uint8_t status;    // 1 = OK
};