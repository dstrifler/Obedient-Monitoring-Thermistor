#ifndef _EEPROM_LAYOUT_H
#define _EEPROM_LAYOUT_H

#include <Arduino.h>
#include <RadioLib.h>

// Total EEPROM reservation
#define EEPROM_TOTAL_BYTES 512

// ------------------------------
// LoRaWAN persistence block
// ------------------------------
#define EEPROM_ADDR_LORAWAN_BASE      0
#define EEPROM_LORAWAN_NONCES_ADDR    EEPROM_ADDR_LORAWAN_BASE
#define EEPROM_LORAWAN_FLAG_ADDR      (EEPROM_LORAWAN_NONCES_ADDR + RADIOLIB_LORAWAN_NONCES_BUF_SIZE)
#define EEPROM_LORAWAN_FLAG_VALUE     0xA5

// ------------------------------
// Application settings block
// ------------------------------
#define EEPROM_ADDR_SETTINGS_BASE     128

#define SETTINGS_MAGIC                0x47534150UL   // "GSAP"
#define SETTINGS_VERSION              1

#endif
