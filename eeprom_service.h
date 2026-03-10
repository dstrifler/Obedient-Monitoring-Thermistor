#ifndef _EEPROM_SERVICE_H
#define _EEPROM_SERVICE_H

#include <Arduino.h>
#include <RadioLib.h>

// Total EEPROM reservation
#define EEPROM_TOTAL_BYTES 512

// ------------------------------
// LoRaWAN persistence block
// ------------------------------
// Expected bytes:
// - Nonce persistence buffer: RADIOLIB_LORAWAN_NONCES_BUF_SIZE bytes
// - Initialization flag:      1 byte
// Keep this block contiguous so RadioLib state remains portable across firmware updates.
#define EEPROM_ADDR_LORAWAN_BASE      0
#define EEPROM_LORAWAN_NONCES_ADDR    EEPROM_ADDR_LORAWAN_BASE
#define EEPROM_LORAWAN_NONCES_BYTES   RADIOLIB_LORAWAN_NONCES_BUF_SIZE
#define EEPROM_LORAWAN_FLAG_ADDR      (EEPROM_LORAWAN_NONCES_ADDR + EEPROM_LORAWAN_NONCES_BYTES)
#define EEPROM_LORAWAN_FLAG_BYTES     1
#define EEPROM_LORAWAN_FLAG_VALUE     0xA5
// Exclusive end of the full LoRaWAN persistence area.
#define EEPROM_ADDR_LORAWAN_END       (EEPROM_LORAWAN_FLAG_ADDR + EEPROM_LORAWAN_FLAG_BYTES)

// ------------------------------
// Application settings block
// ------------------------------
// Expected bytes:
// - Padding to 16-byte boundary: 0..15 bytes (layout hygiene / room for future LoRaWAN growth)
// - Settings reservation:        64 bytes (AppSettings currently uses a small subset)
#define EEPROM_SETTINGS_ALIGN_BYTES    16
#define EEPROM_SETTINGS_PAD_BYTES      ((EEPROM_SETTINGS_ALIGN_BYTES - (EEPROM_ADDR_LORAWAN_END % EEPROM_SETTINGS_ALIGN_BYTES)) % EEPROM_SETTINGS_ALIGN_BYTES)
#define EEPROM_ADDR_SETTINGS_BASE      (EEPROM_ADDR_LORAWAN_END + EEPROM_SETTINGS_PAD_BYTES)
#define EEPROM_SETTINGS_RESERVED_BYTES 64
#define EEPROM_ADDR_SETTINGS_END       (EEPROM_ADDR_SETTINGS_BASE + EEPROM_SETTINGS_RESERVED_BYTES)

// Compile-time guard: fail build if any future layout change would push settings outside
// the configured EEPROM reservation and risk persistence corruption.
#if (EEPROM_ADDR_SETTINGS_END > EEPROM_TOTAL_BYTES)
  #error "EEPROM layout overflow: settings region exceeds EEPROM_TOTAL_BYTES"
#endif

#define SETTINGS_MAGIC                0x47534150UL   // "GSAP"
#define SETTINGS_VERSION              1

#endif
