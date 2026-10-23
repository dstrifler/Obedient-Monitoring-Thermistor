#include "lorawan_service.h"
#include <Arduino.h>
#include <RadioLib.h>
#include <limits.h>
#include <string.h>

#include "config.h"

#if !defined(LORA_USE_OTAA) || (LORA_USE_OTAA != 1)
#error "OTAA required: ABP needs durable session and frame-counter storage."
#endif
#include "nonce_store.h"
#include "lorawan_timer.h"
#include "log_service.h"
#include "radio_hal.h"

static_assert(LW_ERR_RADIO_GPIO != LW_ERR_TIMER_ROLLOVER, "Local radio errors must be distinct");
// NM180100 connects DIO1 to Apollo3 pad 40 (module datasheet table 8).
static_assert(LORA_PIN_IRQ_DIO1 == D40, "NM180100 DIO1 must use Apollo3 D40");
static Apollo3RadioHal gRadioHal(LORA_SPI_BUS, LORA_PIN_IRQ_DIO1, 40);
static SX1262 gRadio = new Module(
  &gRadioHal,
  LORA_PIN_CS,
  LORA_PIN_IRQ_DIO1,
  LORA_PIN_RST,
  LORA_PIN_BUSY
);

static LoRaWANBand_t gRegion = LORA_BAND;
// Pin the serialization contract used for pre-transmit nonce reservations.
static_assert(RADIOLIB_LORAWAN_NONCES_VERSION_VAL == 1, "Review nonce reservation for this RadioLib version");
class PersistentLoRaWANNode : public LoRaWANNode {
public:
  using LoRaWANNode::LoRaWANNode;
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
static int16_t gLastActivationState = RADIOLIB_ERR_NONE;
int16_t lwLastActivationState() { return gLastActivationState; }
bool lwPersistenceHealthy() { return gPersistenceHealthy; }
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


// ============================================================
// LORAWAN INIT
// ============================================================

static bool lwRadioHalHealthy() {
  if(gRadioHal.status() == 0) { return true; }
  LOG_ERROR("RadioGPIO", "DIO1 configuration failed: status=%lu", static_cast<unsigned long>(gRadioHal.status()));
  return false;
}

bool lwBegin() {
  if((LORA_UPLINK_FPORT == 0) || (LORA_UPLINK_FPORT > 223)) {
    LOG_ERROR("LoRaWAN", "Invalid uplink FPort. Must be 1..223.");
    return false;
  }

  if(LORA_USE_SPI != 1) {
    LOG_ERROR("LoRaWAN", "SPI bus disabled or missing in LORA_CONFIG.");
    return false;
  }

  LOG_DEBUG("LoRaWAN", "Initializing radio with XTAL reference.");
  // NM180100 uses a 32 MHz crystal (datasheet section 7.3). Select RadioLib's
  // documented XTAL mode before every init, including recovery, to suppress
  // the default 1.6 V TCXO control on DIO3.
  gRadio.XTAL = true;
  int16_t radioState = gRadio.begin();
  if(!lwRadioHalHealthy()) { return false; }
  if(radioState != RADIOLIB_ERR_NONE) {
    LOG_WARN("LoRaWAN", "Radio initialization failed: code=%d", int(radioState));
    return false;
  }

  // Many SX126x boards (including LoRa Thing Plus variants) route
  // the RF front-end switch to DIO2. If this is not enabled, TX can
  // appear to work in logs while no RF packet is actually radiated.
  int16_t state = gRadio.setDio2AsRfSwitch();
  if((state != RADIOLIB_ERR_NONE) && (state != RADIOLIB_ERR_UNSUPPORTED)) {
    LOG_WARN("LoRaWAN", "RF switch setup failed: code=%d", int(state));
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
    LOG_WARN("LoRaWAN", "Node begin failed: code=%d", int(state));
    return false;
  }

  // Application reports use US915 DR1 (SF9/BW125). Join rates remain under
  // RadioLib's regional rules. Reapply DR1 before each normal transaction:
  // stock RadioLib can process a server LinkADRReq even with ADR disabled.
  gNode.setADR(false);
  LOG_INFO("LoRaWAN", "Radio and OTAA node initialized; normal uplinks use DR1.");
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
    LOG_ERROR("Persistence", "Write/verify failed. Joining disabled.");
  }
  return gPersistenceHealthy;
}

int16_t lwRestore() {
  gPersistenceHealthy = nonceStoreBegin();
  if(!gPersistenceHealthy) { return LW_ERR_PERSISTENCE; }
  const bool haveNonces = nonceStoreLoad(gLWnonces, sizeof(gLWnonces));
  if(haveNonces) {
    int16_t state = gNode.setBufferNonces(gLWnonces);
    if(state != RADIOLIB_ERR_NONE) {
      LOG_ERROR("Persistence", "Nonce restore rejected; history retained. No automatic reset.");
      gPersistenceHealthy = false;
      return state;
    }
  } else {
    // The journal is the only nonce source. Legacy EEPROM is deliberately
    // ignored; lwActivate() must persist a reservation before any join TX.
    LOG_WARN("Persistence", "Nonce journal empty; starting fresh. Legacy EEPROM ignored; prior network nonces may conflict.");
  }
  gReservedNonceLimit = gNode.nextDevNonce();
  LOG_INFO("Persistence", "Nonce history ready; a fresh OTAA session will be joined.");
  return RADIOLIB_ERR_NONE;
}

// ============================================================
// ACTIVATE / JOIN NETWORK
// ============================================================

bool lwRecover() {
  if(!gPersistenceHealthy) { return false; }
  // The scheduler calls this only after its receive-window cooldown. Recreate
  // our node through its constructor, then use the normal public init/restore
  // APIs. No session counters are reused after discarding an uncertain session.
  gNode = PersistentLoRaWANNode(&gRadio, &gRegion, LORA_SUB_BAND);
  if(!lwBegin()) { return false; }
  return lwRestore() == RADIOLIB_ERR_NONE && gPersistenceHealthy;
}

bool lwActivate() {
  int16_t state = RADIOLIB_ERR_NETWORK_NOT_JOINED;
  const uint32_t started = millis();

  if(!lwRadioHalHealthy()) { gLastActivationState = LW_ERR_RADIO_GPIO; return false; }
  if(lwTimerRolloverPending()) { gLastActivationState = LW_ERR_TIMER_ROLLOVER; return false; }

  LOG_INFO("LoRaWAN", "Attempting network join ...");
  gRadio.standby();

#if (LORA_USE_OTAA == 1)
  if(!gPersistenceHealthy) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
  uint16_t next = gNode.nextDevNonce();
  if(next == UINT16_MAX) {
    LOG_ERROR("Persistence", "DevNonce exhausted; explicit reprovisioning required.");
    gLastActivationState = LW_ERR_NONCE_EXHAUSTED;
    return false;
  }
  if(next >= gReservedNonceLimit) {
    // Reserve in batches to reduce wear. A reboot skips unused values.
    uint32_t limit = static_cast<uint32_t>(next) + 16;
    gReservedNonceLimit = limit > UINT16_MAX ? UINT16_MAX : limit;
    if(!lwSaveNonces()) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
  }
  // Flash reservation/logging may have crossed into the guard interval.
  if(millis() < started || lwTimerRolloverPending()) {
    gLastActivationState = LW_ERR_TIMER_ROLLOVER;
    return false;
  }
  gNode.scheduleTransmission(millis());
  state = gNode.activateOTAA();
#else
  state = gNode.activateABP();
#endif

  gLastActivationState = state;
  if(!lwRadioHalHealthy()) {
    // Retain an accepted JoinNonce even if GPIO cleanup failed afterward.
    if(state == RADIOLIB_LORAWAN_NEW_SESSION && !lwSaveNonces()) {
      gLastActivationState = LW_ERR_PERSISTENCE;
      return false;
    }
    gLastActivationState = LW_ERR_RADIO_GPIO;
    return false;
  }

  if(state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    LOG_INFO("LoRaWAN", "Session restored!");
    return true;
  }

  if(state == RADIOLIB_LORAWAN_NEW_SESSION) {
    if(!lwSaveNonces()) { gLastActivationState = LW_ERR_PERSISTENCE; return false; }
    LOG_INFO("LoRaWAN", "Successfully started new session!");
    return true;
  }

#if (LORA_USE_OTAA == 1)
  if(!gNode.isActivated() && gNode.nextDevNonce() == next) {
    // An error before the library's increment does not prove there was no RF.
    // Skip the remainder of the verified reservation using the public restore
    // API. The next attempt must reserve a new batch before transmitting.
    int16_t restoreState = gNode.setBufferNonces(gLWnonces);
    if(restoreState != RADIOLIB_ERR_NONE) {
      gPersistenceHealthy = false;
      gLastActivationState = restoreState;
      LOG_ERROR("Persistence", "Cannot skip uncertain join nonce; joining disabled.");
      return false;
    }
    LOG_WARN("Persistence", "Uncertain join attempt; skipped to reserved nonce bound.");
  }
#endif

  LOG_WARN("LoRaWAN", "Join failed: code=%d", int(state));
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
  if(!lwRadioHalHealthy()) { return LW_ERR_RADIO_GPIO; }
  const uint32_t started = millis();
  LoRaWANEvent_t eventUp = {};
  LoRaWANEvent_t eventDown = {};

  if(millis() < started || lwTimerRolloverPending()) { return LW_ERR_TIMER_ROLLOVER; }

  if(gNode.isActivated()) {
    const int16_t rateState = gNode.setDatarate(1);
    if(rateState != RADIOLIB_ERR_NONE) { return rateState; }
  }

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

  // sendReceive owns TX completion and both Class A receive windows.
  if(!lwRadioHalHealthy()) { *lenDown = 0; return LW_ERR_RADIO_GPIO; }

  if(state >= RADIOLIB_ERR_NONE) {
    LOG_DEBUG("LoRaWAN", "TX frame=%lu freq_kHz=%lu DR=%u receive_window=%d",
      static_cast<unsigned long>(eventUp.fCnt), static_cast<unsigned long>(eventUp.freq * 1000.0f + 0.5f),
      unsigned(eventUp.datarate), int(state));
  }

  if(state <= 0) { *lenDown = 0; }
  return state;
}

// RadioLib owns region, data-rate, pending FOpts and dwell-time calculations.
size_t lwMaxPayloadBytes() {
  if(!gNode.isActivated() || gNode.setDatarate(1) != RADIOLIB_ERR_NONE) { return 0; }
  return gNode.getMaxPayloadLen();
}
uint32_t lwTimeUntilUplink() { return gNode.timeUntilUplink(); }

bool lwTryCapacityDataRate(size_t requiredBytes, uint16_t& triedRates) {
  static_assert(RADIOLIB_LORAWAN_CHANNEL_NUM_DATARATES > 1 && RADIOLIB_LORAWAN_CHANNEL_NUM_DATARATES <= 16,
                "Capacity retry mask needs updating");
  const uint8_t dr = 1;
  const uint16_t bit = 1U << dr;
  if(triedRates & bit) { return false; }
  triedRates |= bit;
  // Keep DR1 fixed. Regional N excludes fixed framing; reserve only FOpts.
  if(gRegion.dataRates[dr] == RADIOLIB_LORAWAN_DATA_RATE_UNUSED ||
     requiredBytes > gRegion.payloadLenMax[dr] ||
     gRegion.payloadLenMax[dr] - requiredBytes < RADIOLIB_LORAWAN_FHDR_FOPTS_MAX_LEN) { return false; }
  const int16_t state = gNode.setDatarate(dr);
  if(state != RADIOLIB_ERR_NONE) {
    LOG_DEBUG("Payload", "DR1 rejected: code=%d", int(state));
    return false;
  }
  const size_t available = gNode.getMaxPayloadLen();
  if(available >= requiredBytes && available <= gRegion.payloadLenMax[dr]) {
    LOG_WARN("Payload", "Retrying at fixed DR1.");
    return true;
  }
  return false;
}
