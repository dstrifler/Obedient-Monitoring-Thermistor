#ifndef _CONFIG_H
#define _CONFIG_H

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// ============================================================
// SERIAL
// ============================================================

#define SERIAL_BAUD_RATE 115200

// ============================================================
// LORA DETAILS FORM
// Fill these values per deployment/board.
// ============================================================

#define LORA_BAND              US915
#define LORA_SUB_BAND          2
#define LORA_VERSION           0   // 0 = LoRaWAN 1.0.x, 1 = LoRaWAN 1.1.x
#define LORA_USE_OTAA          1   // 1 = OTAA, 0 = ABP
#define LORA_UPLINK_FPORT      1

// OTAA keys (fillable form)
#define LORA_JOIN_EUI          0x6081F9C63C1474C3
#define LORA_DEV_EUI           0x6081F9A7215ECB6C
#define LORA_NWK_KEY           \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define LORA_APP_KEY           \
  0x1D, 0x4D, 0x81, 0x2C, 0xE7, 0x6D, 0xED, 0x85, \
  0x06, 0x26, 0x39, 0x9C, 0xB1, 0x9E, 0xAF, 0x58

// ABP keys (fillable form)
#define LORA_DEV_ADDR          0x00000000
#define LORA_NWKS_ENC_KEY      \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define LORA_APP_S_KEY         \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define LORA_FNWKS_INT_KEY     \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#define LORA_SNWKS_INT_KEY     \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

// LoRaWAN reserves FPort 0 for MAC commands.
// Application payload must use ports 1..223.
#if (LORA_UPLINK_FPORT == 0) || (LORA_UPLINK_FPORT > 223)
#error "LORA_UPLINK_FPORT must be in range 1..223 (0 is reserved by LoRaWAN MAC)."
#endif

// ============================================================
// RADIO DETAILS FORM
// Fill these values per deployment/board.
// ============================================================

#define LORA_PIN_CS            D36
#define LORA_PIN_IRQ_DIO1      D40
#define LORA_PIN_RST           D44
#define LORA_PIN_BUSY          D39

#define LORA_USE_SPI           1
#define LORA_SPI_BUS           SPI1

// ============================================================
// SENSOR DETAILS FORM
// Fill these values per deployment/board.
// ============================================================

#define SENSOR_USE_WIRE1       1   // 1 = Wire1, 0 = Wire
#define SENSOR_I2C_ADDR        0x77
#define SENSOR_FORCED_DELAY_MS 250

// how often to send an uplink by default
static constexpr uint32_t DEFAULT_UPLINK_INTERVAL_SECONDS = 15UL * 60UL;

#endif
