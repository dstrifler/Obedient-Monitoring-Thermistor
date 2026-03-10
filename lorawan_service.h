#ifndef _LORAWAN_SERVICE_H
#define _LORAWAN_SERVICE_H

#include <Arduino.h>
#include <RadioLib.h>
#include <EEPROM.h>
#include <string.h>

#include "config.h"
#include "eeprom_service.h"

static SX1262 gRadio = new Module(
  LORA_PIN_CS,
  LORA_PIN_IRQ_DIO1,
  LORA_PIN_RST,
  LORA_PIN_BUSY,
  LORA_SPI_BUS
);

static LoRaWANBand_t gRegion = LORA_BAND;
static LoRaWANNode gNode(&gRadio, &gRegion, LORA_SUB_BAND);

// ============================================================
// LORAWAN KEYS
// ============================================================

#if (LORA_USE_OTAA == 1)

#define RADIOLIB_LORAWAN_JOIN_EUI  0x6081F9C63C1474C3

#ifndef RADIOLIB_LORAWAN_DEV_EUI
#define RADIOLIB_LORAWAN_DEV_EUI   0x6081F9A7215ECB6C
#endif

#ifndef RADIOLIB_LORAWAN_APP_KEY
#define RADIOLIB_LORAWAN_APP_KEY \
  0x1D, 0x4D, 0x81, 0x2C, 0xE7, 0x6D, 0xED, 0x85, \
  0x06, 0x26, 0x39, 0x9C, 0xB1, 0x9E, 0xAF, 0x58
#endif

static uint64_t gJoinEUI = RADIOLIB_LORAWAN_JOIN_EUI;
static uint64_t gDevEUI  = RADIOLIB_LORAWAN_DEV_EUI;
static uint8_t gAppKey[] = { RADIOLIB_LORAWAN_APP_KEY };

#if (LORA_VERSION == 1)
#ifndef RADIOLIB_LORAWAN_NWK_KEY
#define RADIOLIB_LORAWAN_NWK_KEY \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif
static uint8_t gNwkKey[] = { RADIOLIB_LORAWAN_NWK_KEY };
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

static uint32_t gDevAddr = RADIOLIB_LORAWAN_DEV_ADDR;
static uint8_t gSNwkSEncKey[] = { RADIOLIB_LORAWAN_NWKSENC_KEY };
static uint8_t gAppSKey[]     = { RADIOLIB_LORAWAN_APPS_KEY };

#if (LORA_VERSION == 1)
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

static uint8_t gFNwkSIntKey[] = { RADIOLIB_LORAWAN_FNWKSINT_KEY };
static uint8_t gSNwkSIntKey[] = { RADIOLIB_LORAWAN_SNWKSINT_KEY };
#endif

#endif

static uint8_t gLWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

// ============================================================
// LORAWAN INIT
// ============================================================

bool lwBegin() {
  if((LORA_UPLINK_FPORT == 0) || (LORA_UPLINK_FPORT > 223)) {
    Serial.println(F("[LoRaWAN] Invalid uplink FPort. Must be 1..223."));
    return false;
  }

  if(LORA_USE_SPI != 1) {
    Serial.println(F("[LoRaWAN] SPI bus disabled or missing in LORA_CONFIG."));
    return false;
  }

  Serial.print(F("[LoRaWAN] Initialise the radio ... "));
  int16_t radioState = gRadio.begin();
  if(radioState != RADIOLIB_ERR_NONE) {
    Serial.print(F("failed, code "));
    Serial.println(radioState);
    return false;
  }
  Serial.println(F("success!"));

  EEPROM.init();
  EEPROM.setLength(EEPROM_TOTAL_BYTES);

  // Many SX126x boards (including LoRa Thing Plus variants) route
  // the RF front-end switch to DIO2. If this is not enabled, TX can
  // appear to work in logs while no RF packet is actually radiated.
  int16_t state = gRadio.setDio2AsRfSwitch();
  if((state != RADIOLIB_ERR_NONE) && (state != RADIOLIB_ERR_UNSUPPORTED)) {
    Serial.print(F("[LoRaWAN] RF switch setup failed, code "));
    Serial.println(state);
    return false;
  }

#if (LORA_USE_OTAA == 1)
  #if (LORA_VERSION == 1)
    state = gNode.beginOTAA(gJoinEUI, gDevEUI, gNwkKey, gAppKey);
  #else
    state = gNode.beginOTAA(gJoinEUI, gDevEUI, NULL, gAppKey);
  #endif
#else
  #if (LORA_VERSION == 1)
    state = gNode.beginABP(gDevAddr, gFNwkSIntKey, gSNwkSIntKey, gSNwkSEncKey, gAppSKey);
  #else
    state = gNode.beginABP(gDevAddr, NULL, NULL, gSNwkSEncKey, gAppSKey);
  #endif
#endif

  if(state != RADIOLIB_ERR_NONE) {
    Serial.print(F("[LoRaWAN] Node begin failed, code "));
    Serial.println(state);
    return false;
  }

  return true;
}

bool lwIsActivated() {
  return gNode.isActivated();
}

// ============================================================
// RESTORE PERSISTED LORAWAN NONCES
// ============================================================

int16_t lwRestore() {
  int16_t state = RADIOLIB_ERR_UNKNOWN;

  if(EEPROM.read(EEPROM_LORAWAN_FLAG_ADDR) == EEPROM_LORAWAN_FLAG_VALUE) {
    gRadio.standby();
    EEPROM.get(EEPROM_LORAWAN_NONCES_ADDR, gLWnonces);
    state = gNode.setBufferNonces(gLWnonces);

    Serial.print(F("[LoRaWAN] Nonce restore state: "));
    Serial.println(state);
  } else {
    Serial.println(F("[LoRaWAN] No persisted nonces found."));
  }

  return state;
}

// ============================================================
// SAVE CURRENT NONCES TO EEPROM
// ============================================================

static void lwSaveNonces() {
  uint8_t* persist = gNode.getBufferNonces();
  if(persist == nullptr) {
    return;
  }

  memcpy(gLWnonces, persist, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  EEPROM.put(EEPROM_LORAWAN_NONCES_ADDR, gLWnonces);
  EEPROM.write(EEPROM_LORAWAN_FLAG_ADDR, EEPROM_LORAWAN_FLAG_VALUE);
}

// ============================================================
// ACTIVATE / JOIN NETWORK
// ============================================================

bool lwActivate() {
  int16_t state = RADIOLIB_ERR_NETWORK_NOT_JOINED;

  Serial.println(F("[LoRaWAN] Attempting network join ..."));
  gRadio.standby();

#if (LORA_USE_OTAA == 1)
  state = gNode.activateOTAA();
#else
  state = gNode.activateABP();
#endif

  if(state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    Serial.println(F("[LoRaWAN] Session restored!"));
    return true;
  }

  if(state == RADIOLIB_LORAWAN_NEW_SESSION) {
    lwSaveNonces();
    Serial.println(F("[LoRaWAN] Successfully started new session!"));
    return true;
  }

  Serial.print(F("[LoRaWAN] Join failed, code "));
  Serial.println(state);
  return false;
}

// ============================================================
// SEND UPLINK / RECEIVE DOWNLINK
// ============================================================

int16_t lwSendReceive(const uint8_t* dataUp, size_t lenUp, uint8_t* dataDown, size_t* lenDown) {
  if(dataDown == nullptr || lenDown == nullptr) {
    return RADIOLIB_ERR_INVALID_DATA_SHAPING;
  }

  LoRaWANEvent_t eventUp;
  LoRaWANEvent_t eventDown;

  uint8_t* up = (uint8_t*)dataUp;

  int16_t state = gNode.sendReceive(
    up,
    lenUp,
    LORA_UPLINK_FPORT,
    dataDown,
    lenDown,
    false,
    &eventUp,
    &eventDown
  );

  return state;
}

#endif
