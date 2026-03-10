#ifndef _LORAWAN_SERVICE_H
#define _LORAWAN_SERVICE_H

#include <Arduino.h>
#include <RadioLib.h>
#include <EEPROM.h>
#include <string.h>

#include "config.h"
#include "eeprom_layout.h"

#ifndef LORAWAN_UPLINK_FPORT
#define LORAWAN_UPLINK_FPORT 1
#endif

static uint8_t LWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

// ============================================================
// LORAWAN INIT
// ============================================================

bool lwBegin() {
  EEPROM.init();
  EEPROM.setLength(EEPROM_TOTAL_BYTES);

  // Many SX126x boards (including LoRa Thing Plus variants) route
  // the RF front-end switch to DIO2. If this is not enabled, TX can
  // appear to work in logs while no RF packet is actually radiated.
  int16_t state = radio.setDio2AsRfSwitch();
  if((state != RADIOLIB_ERR_NONE) && (state != RADIOLIB_ERR_UNSUPPORTED)) {
    Serial.print(F("[LoRaWAN] RF switch setup failed, code "));
    Serial.println(state);
    return false;
  }

#if (LORAWAN_OTAA == 1)
  #if (LORAWAN_VERSION == 1)
    state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
  #else
    state = node.beginOTAA(joinEUI, devEUI, NULL, appKey);
  #endif
#else
  #if (LORAWAN_VERSION == 1)
    state = node.beginABP(devAddr, fNwkSIntKey, sNwkSIntKey, sNwkSEncKey, appSKey);
  #else
    state = node.beginABP(devAddr, NULL, NULL, sNwkSEncKey, appSKey);
  #endif
#endif

  if(state != RADIOLIB_ERR_NONE) {
    Serial.print(F("[LoRaWAN] Node begin failed, code "));
    Serial.println(state);
    return false;
  }

  return true;
}

// ============================================================
// RESTORE PERSISTED LORAWAN NONCES
// ============================================================

int16_t lwRestore() {
  int16_t state = RADIOLIB_ERR_UNKNOWN;

  if(EEPROM.read(EEPROM_LORAWAN_FLAG_ADDR) == EEPROM_LORAWAN_FLAG_VALUE) {
    radio.standby();
    EEPROM.get(EEPROM_LORAWAN_NONCES_ADDR, LWnonces);
    state = node.setBufferNonces(LWnonces);

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
  uint8_t* persist = node.getBufferNonces();
  if(persist == nullptr) {
    return;
  }

  memcpy(LWnonces, persist, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  EEPROM.put(EEPROM_LORAWAN_NONCES_ADDR, LWnonces);
  EEPROM.write(EEPROM_LORAWAN_FLAG_ADDR, EEPROM_LORAWAN_FLAG_VALUE);
}

// ============================================================
// ACTIVATE / JOIN NETWORK
// ============================================================

bool lwActivate() {
  int16_t state = RADIOLIB_ERR_NETWORK_NOT_JOINED;

  Serial.println(F("[LoRaWAN] Attempting network join ..."));
  radio.standby();

#if (LORAWAN_OTAA == 1)
  state = node.activateOTAA();
#else
  state = node.activateABP();
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

  int16_t state = node.sendReceive(
    up,
    lenUp,
    LORAWAN_UPLINK_FPORT,
    dataDown,
    lenDown,
    false,
    &eventUp,
    &eventDown
  );

  return state;
}

#endif
