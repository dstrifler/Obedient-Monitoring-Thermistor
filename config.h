#ifndef _CONFIG_H
#define _CONFIG_H

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>

// ============================================================
// SERIAL
// ============================================================

#define SERIAL_BAUD_RATE 115200

// ============================================================
// RADIO CONFIGURATION
// SparkFun LoRa Thing Plus expLoRaBLE (Apollo3) + SX1262
// ============================================================

SX1262 radio = new Module(D36, D40, D44, D39, SPI1);

// ============================================================
// LORAWAN STATIC CONFIG
// ============================================================

#define LORAWAN_VERSION  (0)   // Helium uses LoRaWAN 1.0.x
#define LORAWAN_OTAA     (1)   // 1 = OTAA, 0 = ABP

#define LORAWAN_UPLINK_FPORT   1
#define LORAWAN_SCAN_GUARD_MS  100

// how often to send an uplink by default
const uint32_t uplinkIntervalSeconds = 15UL * 60UL;

// regional choices: EU868, US915, AU915, AS923, IN865, KR920, CN780, CN500
const LoRaWANBand_t Region = US915;
const uint8_t subBand = 2;

// ============================================================
// OTAA KEYS
// ============================================================

#if (LORAWAN_OTAA == 1)

#define RADIOLIB_LORAWAN_JOIN_EUI  0x6081F9C63C1474C3

#ifndef RADIOLIB_LORAWAN_DEV_EUI
#define RADIOLIB_LORAWAN_DEV_EUI   0x6081F9A7215ECB6C
#endif

#ifndef RADIOLIB_LORAWAN_APP_KEY
#define RADIOLIB_LORAWAN_APP_KEY \
  0x1D, 0x4D, 0x81, 0x2C, 0xE7, 0x6D, 0xED, 0x85, \
  0x06, 0x26, 0x39, 0x9C, 0xB1, 0x9E, 0xAF, 0x58
#endif

uint64_t joinEUI = RADIOLIB_LORAWAN_JOIN_EUI;
uint64_t devEUI  = RADIOLIB_LORAWAN_DEV_EUI;
uint8_t appKey[] = { RADIOLIB_LORAWAN_APP_KEY };

#if (LORAWAN_VERSION == 1)
#ifndef RADIOLIB_LORAWAN_NWK_KEY
#define RADIOLIB_LORAWAN_NWK_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif
uint8_t nwkKey[] = { RADIOLIB_LORAWAN_NWK_KEY };
#endif

#else

#ifndef RADIOLIB_LORAWAN_DEV_ADDR
#define RADIOLIB_LORAWAN_DEV_ADDR  0x00000000
#endif

#ifndef RADIOLIB_LORAWAN_NWKSENC_KEY
#define RADIOLIB_LORAWAN_NWKSENC_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

#ifndef RADIOLIB_LORAWAN_APPS_KEY
#define RADIOLIB_LORAWAN_APPS_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

uint32_t devAddr = RADIOLIB_LORAWAN_DEV_ADDR;
uint8_t sNwkSEncKey[] = { RADIOLIB_LORAWAN_NWKSENC_KEY };
uint8_t appSKey[]     = { RADIOLIB_LORAWAN_APPS_KEY };

#if (LORAWAN_VERSION == 1)
#ifndef RADIOLIB_LORAWAN_FNWKSINT_KEY
#define RADIOLIB_LORAWAN_FNWKSINT_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

#ifndef RADIOLIB_LORAWAN_SNWKSINT_KEY
#define RADIOLIB_LORAWAN_SNWKSINT_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

uint8_t fNwkSIntKey[] = { RADIOLIB_LORAWAN_FNWKSINT_KEY };
uint8_t sNwkSIntKey[] = { RADIOLIB_LORAWAN_SNWKSINT_KEY };
#endif

#endif

// ============================================================
// LORAWAN NODE
// ============================================================

LoRaWANNode node(&radio, &Region, subBand);

// ============================================================
// APPLICATION STATE
// ============================================================

uint32_t periodicity = uplinkIntervalSeconds;
bool isConfirmed = false;
bool reply = false;

uint8_t dataUp[255];
size_t lenUp = 0;
uint8_t fPort = LORAWAN_UPLINK_FPORT;

// ============================================================
// SENSOR / I2C CONFIGURATION
// Waveshare BME68x on Wire1
// ============================================================

#define SENSOR_USE_WIRE1 1

static constexpr uint8_t BME68X_I2C_ADDR = 0x77;   // change to 0x76 if needed
static constexpr float HUMIDITY_IDEAL = 40.0f;

#define BME68X_FORCED_DELAY_MS 250

// ============================================================
// OPTIONAL DEBUG HELPERS
// ============================================================

void debug(bool isFail, const __FlashStringHelper* message, int state, bool Freeze) {
  if(isFail) {
    Serial.print(message);
    Serial.print("(");
    Serial.print(state);
    Serial.println(")");
    while(Freeze);
  }
}

void arrayDump(uint8_t *buffer, uint16_t len) {
  for(uint16_t c = 0; c < len; c++) {
    char b = buffer[c];
    if(b < 0x10) {
      Serial.print('0');
    }
    Serial.print(b, HEX);
  }
  Serial.println();
}

#endif
