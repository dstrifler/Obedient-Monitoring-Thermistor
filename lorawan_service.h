#ifndef _LORAWAN_SERVICE_H
#define _LORAWAN_SERVICE_H

#include <Arduino.h>
#include <RadioLib.h>
#include <EEPROM.h>
#include <limits.h>
#include <string.h>

#include "config.h"

#if !defined(LORA_USE_OTAA) || (LORA_USE_OTAA != 1)
#error "OTAA required: ABP needs durable session and frame-counter storage."
#endif
#include "clock_service.h"
#include "eeprom_service.h"
#include "nonce_store.h"

#if !defined(RADIOLIB_THERMISTOR_PROTOCOL_FIXES) || (RADIOLIB_THERMISTOR_PROTOCOL_FIXES < 2)
#error "Use tools/check-board.sh: RadioLib 7.1.2 needs the checked protocol fixes."
#endif

static SX1262 gRadio = new Module(
  LORA_PIN_CS,
  LORA_PIN_IRQ_DIO1,
  LORA_PIN_RST,
  LORA_PIN_BUSY,
  LORA_SPI_BUS
);

static LoRaWANBand_t gRegion = LORA_BAND;
// Pin the serialization contract used for pre-transmit nonce reservations.
static_assert(RADIOLIB_LORAWAN_NONCES_VERSION_VAL == 1, "Review nonce reservation for this RadioLib version");
class PersistentLoRaWANNode : public LoRaWANNode {
public:
  using LoRaWANNode::LoRaWANNode;
  bool macCommandsFit() {
    // Ask the patched library for total capacity before subtracting FOpts.
    uint8_t pending = fOptsUpLen;
    fOptsUpLen = 0;
    size_t capacity = getMaxPayloadLen();
    fOptsUpLen = pending;
    return pending <= capacity;
  }
  uint16_t nextDevNonce() const { return devNonce; }
  void reservedNonces(uint8_t* out, uint16_t highWater) {
    memcpy(out, getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    hton<uint16_t>(&out[RADIOLIB_LORAWAN_NONCES_DEV_NONCE], highWater);
    hton<uint16_t>(&out[RADIOLIB_LORAWAN_NONCES_SIGNATURE],
      checkSum16(out, RADIOLIB_LORAWAN_NONCES_BUF_SIZE - 2));
  }
};
static PersistentLoRaWANNode gNode(&gRadio, &gRegion, LORA_SUB_BAND);
static uint16_t gReservedNonceLimit = 0;
static bool gPersistenceHealthy = true;
static const int16_t LW_ERR_PERSISTENCE = -32000;
static const int16_t LW_ERR_NONCE_EXHAUSTED = -31999;
static int16_t gLastActivationState = RADIOLIB_ERR_NONE;
static int16_t lwLastActivationState() { return gLastActivationState; }
static bool lwPersistenceHealthy() { return gPersistenceHealthy; }
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

static bool lwSaveNonces() {
  // Persist the reserved upper bound, never the lower in-RAM next nonce.
  gNode.reservedNonces(gLWnonces, gReservedNonceLimit);
  gPersistenceHealthy = nonceStoreSave(gLWnonces);
  if(!gPersistenceHealthy) {
    Serial.println(F("[Persistence] Write/verify failed. Joining disabled."));
  }
  return gPersistenceHealthy;
}

int16_t lwRestore() {
  gPersistenceHealthy = nonceStoreBegin();
  if(!gPersistenceHealthy) { return LW_ERR_PERSISTENCE; }
  bool haveNonces = gNonceStoreHasRecord;
  if(haveNonces) {
    memcpy(gLWnonces, gNonceSaved, sizeof(gLWnonces));
  } else {
    // The compatibility EEPROM convenience methods discard read errors.
    // Use its underlying block device when migrating old records.
    uint8_t legacy[EEPROM_LORAWAN_NONCES_BYTES + 1];
    if(EEPROM.FlashIAPBlockDevice::read(legacy, 0, sizeof(legacy)) != 0) {
      gPersistenceHealthy = false;
      return LW_ERR_PERSISTENCE;
    }
    if(legacy[EEPROM_LORAWAN_FLAG_ADDR] == EEPROM_LORAWAN_FLAG_VALUE) {
      memcpy(gLWnonces, legacy, sizeof(gLWnonces));
      haveNonces = true;
      Serial.println(F("[Persistence] Migrating legacy nonce history; prior unsaved attempts cannot be recovered."));
    } else {
      for(size_t i = 0; i < sizeof(legacy); ++i) {
        if(legacy[i] != 0xFF) {
          Serial.println(F("[Persistence] Unrecognized legacy data; explicit recovery required."));
          gPersistenceHealthy = false;
          return LW_ERR_PERSISTENCE;
        }
      }
    }
  }
  if(haveNonces) {
    int16_t state = gNode.setBufferNonces(gLWnonces);
    if(state != RADIOLIB_ERR_NONE) {
      Serial.println(F("[Persistence] Nonce restore rejected; history retained. No automatic reset."));
      gPersistenceHealthy = false;
      return state;
    }
  }
  gReservedNonceLimit = gNode.nextDevNonce();
  if(haveNonces && !gNonceStoreHasRecord && !lwSaveNonces()) { return LW_ERR_PERSISTENCE; }
  Serial.println(F("[Persistence] Nonce history ready; a fresh OTAA session will be joined."));
  return RADIOLIB_ERR_NONE;
}

// ============================================================
// ACTIVATE / JOIN NETWORK
// ============================================================

bool lwActivate() {
  int16_t state = RADIOLIB_ERR_NETWORK_NOT_JOINED;

  Serial.println(F("[LoRaWAN] Attempting network join ..."));
  gRadio.standby();
  clockLogTimestamp(F("OTAA/ABP activation begin"));

#if (LORA_USE_OTAA == 1)
  if(!gPersistenceHealthy) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
  uint16_t next = gNode.nextDevNonce();
  if(next == UINT16_MAX) {
    Serial.println(F("[Persistence] DevNonce exhausted; explicit reprovisioning required."));
    gLastActivationState = LW_ERR_NONCE_EXHAUSTED;
    return false;
  }
  if(next >= gReservedNonceLimit) {
    // Reserve in batches to reduce wear. A reboot skips unused values.
    uint32_t limit = static_cast<uint32_t>(next) + 16;
    gReservedNonceLimit = limit > UINT16_MAX ? UINT16_MAX : limit;
    if(!lwSaveNonces()) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
  }
  gNode.scheduleTransmission(millis());
  state = gNode.activateOTAA();
#else
  state = gNode.activateABP();
#endif

  gLastActivationState = state;

  // RadioLib owns the Join Request TX and RX1/RX2 scheduling. These boundary
  // timestamps bracket that blocking transaction for bench logs.
  clockLogTimestamp(F("OTAA/ABP activation complete"));

  if(state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    Serial.println(F("[LoRaWAN] Session restored!"));
    return true;
  }

  if(state == RADIOLIB_LORAWAN_NEW_SESSION) {
    if(!lwSaveNonces()) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
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

  *lenDown = 0;
  LoRaWANEvent_t eventUp = {};
  LoRaWANEvent_t eventDown = {};

  Serial.print(F("[LoRaWAN] Uplink preflight: maxPayload="));
  Serial.print(lwMaxPayloadBytes());
  Serial.print(F(", len=")); Serial.println(lenUp);

  clockLogTimestamp(F("uplink transaction begin"));

  int16_t state = gNode.sendReceive(
    dataUp,
    lenUp,
    LORA_UPLINK_FPORT,
    dataDown,
    lenDown,
    false,
    &eventUp,
    &eventDown
  );

  // sendReceive includes TX completion and both Class A receive windows;
  // RadioLib remains solely responsible for their timing.
  clockLogTimestamp(F("uplink transaction complete"));

  if(state >= RADIOLIB_ERR_NONE) {
    Serial.print(F("[LoRaWAN] TX frame=")); Serial.print(eventUp.fCnt);
    Serial.print(F(", MHz=")); Serial.print(eventUp.freq);
    Serial.print(F(", DR=")); Serial.print(eventUp.datarate);
    Serial.print(F(", receive window=")); Serial.println(state);
  }

  if(state <= 0) { *lenDown = 0; }
  return state;
}

// RadioLib owns region, data-rate, pending FOpts and dwell-time calculations.
size_t lwMaxPayloadBytes() { return gNode.getMaxPayloadLen(); }
static bool lwMacCommandsFit() { return gNode.macCommandsFit(); }
static uint32_t lwTimeUntilUplink() { return gNode.timeUntilUplink(); }

#endif
