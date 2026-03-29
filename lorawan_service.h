#ifndef _LORAWAN_SERVICE_H
#define _LORAWAN_SERVICE_H

#include <Arduino.h>
#include <RadioLib.h>
#include <EEPROM.h>
#include <limits.h>
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
static uint8_t gCurrentDataRate = 0;

#ifndef LORA_DEFAULT_DATA_RATE
#define LORA_DEFAULT_DATA_RATE 0
#endif

// ============================================================
// LORAWAN KEYS
// ============================================================

#if (LORA_USE_OTAA == 1)

static uint64_t gJoinEUI = LORA_JOIN_EUI;
static uint64_t gDevEUI  = LORA_DEV_EUI;
static uint8_t gAppKey[] = { LORA_APP_KEY };

#if (LORA_VERSION == 1)
static uint8_t gNwkKey[] = { LORA_NWK_KEY };
#endif

#else

static uint32_t gDevAddr = LORA_DEV_ADDR;
static uint8_t gSNwkSEncKey[] = { LORA_NWKS_ENC_KEY };
static uint8_t gAppSKey[]     = { LORA_APP_S_KEY };

#if (LORA_VERSION == 1)
static uint8_t gFNwkSIntKey[] = { LORA_FNWKS_INT_KEY };
static uint8_t gSNwkSIntKey[] = { LORA_SNWKS_INT_KEY };
#endif

#endif

static uint8_t gLWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

size_t lwMaxPayloadBytes();

// Wrapper policy for local preflight parameter validation:
// - Prefer a generic RadioLib invalid-argument error code when available.
// - If not provided by the bundled RadioLib version, use a dedicated local
//   code so caller logs can clearly separate API misuse from RF/join failures.
#if defined(RADIOLIB_ERR_INVALID_ARGUMENT)
static const int16_t LW_ERR_INVALID_ARGUMENT = RADIOLIB_ERR_INVALID_ARGUMENT;
#elif defined(RADIOLIB_ERR_INVALID_PARAM)
static const int16_t LW_ERR_INVALID_ARGUMENT = RADIOLIB_ERR_INVALID_PARAM;
#else
static const int16_t LW_ERR_INVALID_ARGUMENT = (int16_t)(INT16_MIN + 42);
#endif

static void lwInvalidatePersistedNonces(bool wipeNonces) {
  if(wipeNonces) {
    memset(gLWnonces, 0, sizeof(gLWnonces));
    EEPROM.put(EEPROM_LORAWAN_NONCES_ADDR, gLWnonces);
  }

  EEPROM.write(EEPROM_LORAWAN_FLAG_ADDR, 0x00);
}

template <typename T>
static auto lwReadNodeDataRateImpl(T& node, int) -> decltype(node.getDataRate(), int16_t()) {
  return node.getDataRate();
}

template <typename T>
static auto lwReadNodeDataRateImpl(T& node, long) -> decltype(node.getDatarate(), int16_t()) {
  return node.getDatarate();
}

template <typename T>
static int16_t lwReadNodeDataRateImpl(T&, ...) {
  return RADIOLIB_ERR_UNSUPPORTED;
}

static int16_t lwReadNodeDataRate() {
  return lwReadNodeDataRateImpl(gNode, 0);
}

static void lwRefreshCurrentDataRate(const LoRaWANEvent_t* eventMeta, const __FlashStringHelper* reason) {
  bool refreshed = false;
  uint8_t newDataRate = gCurrentDataRate;

  int16_t nodeDataRate = lwReadNodeDataRate();
  if((nodeDataRate >= 0) && (nodeDataRate <= 15)) {
    newDataRate = (uint8_t)nodeDataRate;
    refreshed = true;
  } else if((eventMeta != nullptr) && (eventMeta->datarate <= 15)) {
    newDataRate = eventMeta->datarate;
    refreshed = true;
  }

  if(!refreshed) {
    return;
  }

  gCurrentDataRate = newDataRate;

  Serial.print(F("[LoRaWAN] DR sync "));
  Serial.print(reason);
  Serial.print(F(": DR"));
  Serial.print(gCurrentDataRate);
  Serial.print(F(", maxPayload="));
  Serial.println(lwMaxPayloadBytes());
}

// ============================================================
// LORAWAN INIT
// ============================================================

bool lwBegin() {
  gCurrentDataRate = LORA_DEFAULT_DATA_RATE;

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

  // EEPROM must already be initialized by app bootstrap (eepromBootstrap()).

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
    bool nonceBufferLoaded = (state == RADIOLIB_ERR_NONE);

    if(nonceBufferLoaded) {
      Serial.println(F("[LoRaWAN] Nonce buffer load accepted (RADIOLIB_ERR_NONE)."));
    } else {
      lwInvalidatePersistedNonces(true);
      Serial.print(F("[LoRaWAN] Nonce buffer load failed (error state "));
      Serial.print(state);
      Serial.println(F("). Invalidated persisted nonces; a fresh session will be created."));
    }

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
    lwRefreshCurrentDataRate(nullptr, F("(session-restored)"));
    Serial.println(F("[LoRaWAN] Session restored!"));
    return true;
  }

  if(state == RADIOLIB_LORAWAN_NEW_SESSION) {
    lwSaveNonces();
    lwRefreshCurrentDataRate(nullptr, F("(new-session)"));
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
    return LW_ERR_INVALID_ARGUMENT;
  }

  if((dataUp == nullptr) && (lenUp > 0)) {
    return LW_ERR_INVALID_ARGUMENT;
  }

  LoRaWANEvent_t eventUp;
  LoRaWANEvent_t eventDown;

  uint8_t* up = (uint8_t*)dataUp;

  lwRefreshCurrentDataRate(nullptr, F("(pre-send)"));
  Serial.print(F("[LoRaWAN] Uplink preflight: DR"));
  Serial.print(gCurrentDataRate);
  Serial.print(F(", maxPayload="));
  Serial.print(lwMaxPayloadBytes());
  Serial.print(F(", len="));
  Serial.println(lenUp);

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

  if(state >= RADIOLIB_ERR_NONE) {
    lwRefreshCurrentDataRate(&eventUp, F("(post-uplink)"));
  }

  return state;
}

// ============================================================
// REGION / DATA-RATE PAYLOAD LIMIT HELPERS
// ============================================================
//
// Returns the maximum FRMPayload bytes available to the application at a
// given data-rate for this firmware's region build. Values are conservative
// and include room for LoRaWAN MAC/FOpts overhead in typical Class A uplinks.
// Unknown regions/data-rates fail closed to 0 so caller can safely drop/fallback.

uint8_t lwGetCurrentDataRate() {
  return gCurrentDataRate;
}

void lwSetCurrentDataRate(uint8_t dataRate) {
  gCurrentDataRate = dataRate;
}

size_t lwMaxPayloadBytesFor(LoRaWANBand_t region, uint8_t dataRate) {
  (void)region;
#if (LORA_BAND == US915) || (LORA_BAND == AU915)
  switch(dataRate) {
    case 0:  return 11;
    case 1:  return 53;
    case 2:  return 125;
    case 3:  return 242;
    case 4:  return 242;
    case 8:  return 53;
    case 9:  return 129;
    case 10: return 242;
    case 11: return 242;
    case 12: return 242;
    case 13: return 242;
    default: return 0;
  }
#elif (LORA_BAND == EU868) || (LORA_BAND == IN865) || (LORA_BAND == RU864)
  switch(dataRate) {
    case 0:
    case 1:
    case 2:  return 51;
    case 3:  return 115;
    case 4:
    case 5:
    case 6:
    case 7:  return 222;
    default: return 0;
  }
#elif (LORA_BAND == AS923) || (LORA_BAND == KR920)
  switch(dataRate) {
    case 0:
    case 1:
    case 2:  return 51;
    case 3:  return 115;
    case 4:
    case 5:
    case 6:
    case 7:  return 222;
    default: return 0;
  }
#else
  return 0;
#endif
}

size_t lwMaxPayloadBytes() {
  uint8_t activeDataRate = (gCurrentDataRate <= 15) ? gCurrentDataRate : LORA_DEFAULT_DATA_RATE;
  return lwMaxPayloadBytesFor(gRegion, activeDataRate);
}

#endif
