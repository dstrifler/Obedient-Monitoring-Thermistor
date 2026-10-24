#include "lorawan_service.h"
#include "storage_service.h"
#include "log_service.h"
#include "config.h"
#include <Arduino.h>
#include <hal/Arduino/ArduinoHal.h>
#include <am_mcu_apollo.h>
#include <platform/mbed_critical.h>
#include <limits.h>
#include <string.h>

// Private Apollo3 radio HAL.

namespace {
// Firmware-owned Apollo3 GPIO handling through RadioLib's public HAL interface.
// All SPI, timing and radio protocol operations remain in stock RadioLib.
class Apollo3RadioHal : public ArduinoHal {
public:
  Apollo3RadioHal(SPIClass& spi, uint32_t dio1Pin, uint32_t dio1Pad);
  void init() override;
  void pinMode(uint32_t pin, uint32_t mode) override;
  void attachInterrupt(uint32_t interruptNum, void (*callback)(void), uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;
  uint32_t status() const;

private:
  void recordStatus(uint32_t result);
  bool maskDio1Interrupt();  // Masks just DIO1; releases the global critical section.
  void restoreDio1Polling();  // Caller holds the critical section.
  const uint32_t dio1Pin_;
  const uint32_t dio1Pad_;
  uint32_t status_ = 0;
};

Apollo3RadioHal::Apollo3RadioHal(SPIClass& spi, uint32_t dio1Pin, uint32_t dio1Pad)
  : ArduinoHal(spi), dio1Pin_(dio1Pin), dio1Pad_(dio1Pad) {}

void Apollo3RadioHal::init() {
  status_ = AM_HAL_STATUS_SUCCESS;
  ArduinoHal::init();
}

void Apollo3RadioHal::recordStatus(uint32_t result) {
  if(status_ == AM_HAL_STATUS_SUCCESS && result != AM_HAL_STATUS_SUCCESS) {
    status_ = result;
  }
}

uint32_t Apollo3RadioHal::status() const { return status_; }

bool Apollo3RadioHal::maskDio1Interrupt() {
  if(dio1Pad_ >= AM_HAL_GPIO_MAX_PADS) {
    recordStatus(AM_HAL_STATUS_INVALID_ARG);
    return false;
  }
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  core_util_critical_section_enter();
  recordStatus(am_hal_gpio_interrupt_disable(mask));
  recordStatus(am_hal_gpio_interrupt_clear(mask));
  core_util_critical_section_exit();
  return true;
}

void Apollo3RadioHal::restoreDio1Polling() {
  if(dio1Pad_ >= AM_HAL_GPIO_MAX_PADS) {
    recordStatus(AM_HAL_STATUS_INVALID_ARG);
    return;
  }
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  recordStatus(am_hal_gpio_interrupt_disable(mask));
  // Apollo3 INTDIR_NONE forces input reads to zero. A complete input config
  // explicitly restores READPIN; ordinary Arduino pinMode(INPUT) need not.
  // Leave the hardware IRQ masked while no receive callback is registered.
  recordStatus(am_hal_gpio_pinconfig(dio1Pad_, g_AM_HAL_GPIO_INPUT));
  recordStatus(am_hal_gpio_interrupt_clear(mask));
}

void Apollo3RadioHal::pinMode(uint32_t pin, uint32_t mode) {
  ArduinoHal::pinMode(pin, mode);
  if(pin == dio1Pin_ && mode == GpioModeInput) {
    core_util_critical_section_enter();
    restoreDio1Polling();
    core_util_critical_section_exit();
  }
}

void Apollo3RadioHal::detachInterrupt(uint32_t interruptNum) {
  if(interruptNum != pinToInterrupt(dio1Pin_)) {
    ArduinoHal::detachInterrupt(interruptNum);
    return;
  }
  if(!maskDio1Interrupt()) { return; }
  // Arduino callback management may allocate or lock an RTOS mutex. Keep it
  // outside global critical sections, with just the DIO1 hardware IRQ masked.
  ArduinoHal::detachInterrupt(interruptNum);
  core_util_critical_section_enter();
  restoreDio1Polling();
  core_util_critical_section_exit();
}

void Apollo3RadioHal::attachInterrupt(uint32_t interruptNum, void (*callback)(void), uint32_t mode) {
  if(interruptNum != pinToInterrupt(dio1Pin_)) {
    ArduinoHal::attachInterrupt(interruptNum, callback, mode);
    return;
  }
  if(callback == nullptr) {
    detachInterrupt(interruptNum);
    return;
  }
  if(!maskDio1Interrupt()) { return; }
  // In particular, the first attach constructs InterruptIn and can allocate.
  ArduinoHal::attachInterrupt(interruptNum, callback, mode);
  const uint64_t mask = uint64_t(1) << dio1Pad_;
  core_util_critical_section_enter();
  recordStatus(am_hal_gpio_interrupt_clear(mask));
  if(status_ == AM_HAL_STATUS_SUCCESS) {
    recordStatus(am_hal_gpio_interrupt_enable(mask));
  }
  core_util_critical_section_exit();
}

} // namespace

// Private LoRa rollover timing.
namespace {

// RadioLib 7.1.2 uses absolute uint32_t timestamps inside blocking calls.
// Leave ten minutes for a complete US915 Class A transaction, including up to
// 15 NbTrans repetitions and the maximum 16-second RX2 delay, plus PHY/CPU
// margin. Do not start another transaction in this interval. Revisit this
// budget when changing region, modem, or the library's blocking behavior.
static constexpr uint32_t LW_TRANSACTION_GUARD_MS = 10UL * 60UL * 1000UL;

bool lwTimerRolloverPending();

bool lwTimerRolloverPending() {
  return millis() >= UINT32_MAX - LW_TRANSACTION_GUARD_MS;
}
} // namespace

// LoRa transport and lifecycle.
namespace {
int16_t lwBegin();
bool lwIsActivated();
int16_t lwRestore();
int16_t lwRecover();
bool lwActivate();
int16_t lwLastActivationState();
bool lwPersistenceHealthy();
int16_t lwSendReceive(const uint8_t* dataUp, size_t lenUp, uint8_t* dataDown, size_t* lenDown);
size_t lwMaxPayloadBytes();
uint32_t lwTimeUntilUplink();
bool lwTryCapacityDataRate(size_t requiredBytes, uint16_t& triedRates);

#if !defined(LORA_USE_OTAA) || (LORA_USE_OTAA != 1)
#error "OTAA required: ABP needs durable session and frame-counter storage."
#endif

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

// Return the failing initialization step's error, or NONE after full setup.
int16_t lwBegin() {
  if((LORA_UPLINK_FPORT == 0) || (LORA_UPLINK_FPORT > 223)) {
    LOG_ERROR("LoRaWAN", "Invalid uplink FPort. Must be 1..223.");
    return RADIOLIB_ERR_INVALID_PORT;
  }

  if(LORA_USE_SPI != 1) {
    LOG_ERROR("LoRaWAN", "SPI bus disabled or missing in LORA_CONFIG.");
    return RADIOLIB_ERR_INVALID_MODE;
  }

  LOG_DEBUG("LoRaWAN", "Initializing radio with XTAL reference.");
  // NM180100 uses a 32 MHz crystal (datasheet section 7.3). Select RadioLib's
  // documented XTAL mode before every init, including recovery, to suppress
  // the default 1.6 V TCXO control on DIO3.
  gRadio.XTAL = true;
  int16_t radioState = gRadio.begin();
  if(!lwRadioHalHealthy()) { return LW_ERR_RADIO_GPIO; }
  if(radioState != RADIOLIB_ERR_NONE) {
    LOG_WARN("LoRaWAN", "Radio initialization failed: code=%d", int(radioState));
    return radioState;
  }

  // Many SX126x boards (including LoRa Thing Plus variants) route
  // the RF front-end switch to DIO2. If this is not enabled, TX can
  // appear to work in logs while no RF packet is actually radiated.
  int16_t state = gRadio.setDio2AsRfSwitch();
  if((state != RADIOLIB_ERR_NONE) && (state != RADIOLIB_ERR_UNSUPPORTED)) {
    LOG_WARN("LoRaWAN", "RF switch setup failed: code=%d", int(state));
    return state;
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
    return state;
  }

  // Application reports use US915 DR1 (SF9/BW125). Join rates remain under
  // RadioLib's regional rules. Reapply DR1 before each normal transaction:
  // stock RadioLib can process a server LinkADRReq even with ADR disabled.
  gNode.setADR(false);
  LOG_INFO("LoRaWAN", "Radio and OTAA node initialized; normal uplinks use DR1.");
  return RADIOLIB_ERR_NONE;
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
  gPersistenceHealthy = storageNonceSave(gLWnonces);
  if(!gPersistenceHealthy) {
    LOG_ERROR("Persistence", "Write/verify failed. Joining disabled.");
  }
  return gPersistenceHealthy;
}

int16_t lwRestore() {
  gPersistenceHealthy = storageNonceBegin();
  if(!gPersistenceHealthy) { return LW_ERR_PERSISTENCE; }
  const bool haveNonces = storageNonceLoad(gLWnonces, sizeof(gLWnonces));
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

int16_t lwRecover() {
  if(!gPersistenceHealthy) { return LW_ERR_PERSISTENCE; }
  // The scheduler calls this only after its receive-window cooldown. Recreate
  // our node through its constructor, then use the normal public init/restore
  // APIs. No session counters are reused after discarding an uncertain session.
  gNode = PersistentLoRaWANNode(&gRadio, &gRegion, LORA_SUB_BAND);
  const int16_t initialized = lwBegin();
  if(initialized != RADIOLIB_ERR_NONE) { return initialized; }
  return lwRestore();
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
  else if(*lenDown && eventDown.fPort != LORA_UPLINK_FPORT) {
    // RadioLib already processed MAC traffic. Only our application port may
    // supply configuration/reset commands to management.
    LOG_WARN("LoRaWAN", "Ignoring application downlink on FPort %u.", unsigned(eventDown.fPort));
    *lenDown = 0;
  }
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

} // namespace

// Radio lifecycle is kept separate from application reporting deadlines. All
// calls execute on main, outside interrupt context and after clock verification.
namespace {
RadioState serviceState = RadioState::Stopped;
uint32_t serviceTick = 0, phaseStarted = 0, lastJoin = 0;
int16_t serviceError = 0;
uint8_t initAttempts = 0, recoveryAttempts = 0, joinFailures = 0, txFailures = 0;
uint8_t capacityFailures = 0, unavailableFailures = 0;
uint16_t capacityTried = 0;
bool joinedAttempted = false;
constexpr uint32_t RECOVERY_MS = 60000UL;

bool hardwareError(int16_t error) {
  return error == RADIOLIB_ERR_TX_TIMEOUT || error == RADIOLIB_ERR_RX_TIMEOUT ||
    error == RADIOLIB_ERR_SPI_WRITE_FAILED || error == RADIOLIB_ERR_SPI_CMD_TIMEOUT ||
    error == RADIOLIB_ERR_SPI_CMD_INVALID || error == RADIOLIB_ERR_SPI_CMD_FAILED ||
    error == RADIOLIB_ERR_CHIP_NOT_FOUND;
}
bool permanentError(int16_t error) {
  return error == LW_ERR_INVALID_ARGUMENT || error == LW_ERR_PERSISTENCE ||
    error == LW_ERR_NONCE_EXHAUSTED || error == LW_ERR_RADIO_GPIO ||
    error == RADIOLIB_ERR_NULL_POINTER || error == RADIOLIB_ERR_INVALID_PORT ||
    error == RADIOLIB_ERR_INVALID_REVISION || error == RADIOLIB_ERR_INVALID_MODE ||
    error == RADIOLIB_ERR_INVALID_DATA_RATE || error == RADIOLIB_ERR_INVALID_FREQUENCY;
}
void radioFault(int16_t error) {
  serviceState = RadioState::Fault; serviceError = error;
  LOG_ERROR("LoRaWAN", "Radio paused: code=%d; resolve before restarting.", int(error));
}
void cooldown() {
  serviceState = RadioState::Cooldown; phaseStarted = millis(); recoveryAttempts = 0;
  txFailures = joinFailures = capacityFailures = unavailableFailures = 0;
  LOG_WARN("LoRaWAN", "Cooling down before radio recovery.");
}
void rolloverWait() {
  const uint32_t now = millis();
  if(now < UINT32_MAX - LW_TRANSACTION_GUARD_MS) { cooldown(); return; }
  serviceState = RadioState::Rollover; phaseStarted = now;
  LOG_INFO("LoRaWAN", "Waiting for timer rollover before radio recovery.");
}
}
void lwServiceStart() {
  serviceState = RadioState::Initializing; serviceTick = phaseStarted = millis();
  initAttempts = recoveryAttempts = joinFailures = txFailures = capacityFailures = unavailableFailures = 0;
  capacityTried = 0; serviceError = 0; joinedAttempted = false;
}
void lwServiceStop() { serviceState = RadioState::Stopped; }
RadioState lwServiceState() { return serviceState; }
int16_t lwServiceError() { return serviceError; }
bool lwServiceReady() { return serviceState == RadioState::Ready && lwIsActivated(); }
void lwServicePoll() {
  const uint32_t now = millis();
  if(serviceState == RadioState::Stopped || serviceState == RadioState::Fault) { return; }
  if(serviceState != RadioState::Rollover && (now < serviceTick || lwTimerRolloverPending())) {
    if(now < serviceTick) { cooldown(); } else { rolloverWait(); }
  }
  serviceTick = now;
  switch(serviceState) {
    case RadioState::Initializing: {
      if(initAttempts && uint32_t(now - phaseStarted) < (1000UL << (initAttempts - 1))) { return; }
      const int16_t initialized = lwBegin();
      if(initialized == RADIOLIB_ERR_NONE) {
        const int16_t restored = lwRestore();
        if(!lwPersistenceHealthy() || restored < 0) { radioFault(restored); return; }
        serviceState = RadioState::Joining; joinedAttempted = false;
      } else {
        serviceError = initialized;
        if(++initAttempts >= 3) { radioFault(initialized); }
      }
      phaseStarted = millis();
      return;
    }
    case RadioState::Joining:
      if(lwIsActivated()) { serviceState = RadioState::Ready; return; }
      if(joinedAttempted && uint32_t(now - lastJoin) < 60000UL) { return; }
      joinedAttempted = true;
      if(lwActivate() && lwIsActivated()) {
        serviceState = RadioState::Ready; serviceError = 0;
        joinFailures = txFailures = 0;
        LOG_INFO("LoRaWAN", "Joined; scheduler owns sensor delivery timing.");
      } else {
        serviceError = lwLastActivationState();
        if(permanentError(serviceError) || !lwPersistenceHealthy()) { radioFault(serviceError); }
        else if(serviceError == LW_ERR_TIMER_ROLLOVER) { rolloverWait(); }
        else if(hardwareError(serviceError)) { if(++joinFailures >= 3) { cooldown(); } }
        else { joinFailures = 0; }
      }
      lastJoin = millis(); return;
    case RadioState::Ready:
      if(!lwIsActivated()) { serviceState = RadioState::Joining; joinedAttempted = false; }
      return;
    case RadioState::Rollover:
      if(now < phaseStarted) { cooldown(); }
      return;
    case RadioState::Cooldown: {
      if(uint32_t(now - phaseStarted) < RECOVERY_MS) { return; }
      const int16_t recovered = lwRecover();
      if(recovered == RADIOLIB_ERR_NONE) { serviceState = RadioState::Joining; joinedAttempted = false; capacityTried = 0; }
      else {
        // A radio/SPI failure must not be mislabeled as lost nonce history.
        serviceError = recovered;
        if(!lwPersistenceHealthy() || ++recoveryAttempts >= 3) { radioFault(recovered); }
      }
      phaseStarted = millis(); return;
    }
    default: return;
  }
}
uint32_t lwServiceWait() {
  if(!lwServiceReady()) { return UINT32_MAX; }
  const uint32_t wait = lwTimeUntilUplink();
  if(wait > INT32_MAX) { cooldown(); return UINT32_MAX; }
  return wait;
}
RadioTransaction lwTransmit(const uint8_t* payload, size_t size) {
  RadioTransaction transaction = {};
  transaction.result = RADIOLIB_ERR_NETWORK_NOT_JOINED;
  if(!lwServiceReady()) { return transaction; }
  if(!payload || !size) { transaction.result = LW_ERR_INVALID_ARGUMENT; return transaction; }
  transaction.macOnly = size > lwMaxPayloadBytes();
  transaction.result = lwSendReceive(payload, transaction.macOnly ? 0 : size,
    transaction.downlink, &transaction.downlinkSize);
  transaction.applicationSent = transaction.result >= 0 && !transaction.macOnly;
  serviceError = transaction.result;
  if(transaction.result < 0) {
    if(!hardwareError(transaction.result)) { txFailures = 0; }
    if(transaction.result != RADIOLIB_ERR_PACKET_TOO_LONG && transaction.result != RADIOLIB_ERR_DWELL_TIME_EXCEEDED) { capacityFailures = 0; }
    if(transaction.result != RADIOLIB_ERR_UPLINK_UNAVAILABLE) { unavailableFailures = 0; }
  }
  if(transaction.result >= 0) {
    txFailures = capacityFailures = unavailableFailures = 0; capacityTried = 0;
    LOG_INFO("LoRaWAN", "Transaction complete: code=%d, application=%u.",
      int(transaction.result), unsigned(transaction.applicationSent));
  } else if(permanentError(transaction.result)) { radioFault(transaction.result); }
  else if(transaction.result == LW_ERR_TIMER_ROLLOVER) { rolloverWait(); }
  else if(transaction.result == RADIOLIB_ERR_NETWORK_NOT_JOINED) { serviceState = RadioState::Joining; joinedAttempted = false; }
  else if(hardwareError(transaction.result)) { if(++txFailures >= 3) { cooldown(); } }
  else if(transaction.result == RADIOLIB_ERR_PACKET_TOO_LONG || transaction.result == RADIOLIB_ERR_DWELL_TIME_EXCEEDED) {
    txFailures = 0;
    if(++capacityFailures >= 3) {
      capacityFailures = 0;
      if(!lwTryCapacityDataRate(size, capacityTried)) { radioFault(RADIOLIB_ERR_INVALID_DATA_RATE); }
    }
  } else if(transaction.result == RADIOLIB_ERR_UPLINK_UNAVAILABLE && !lwTimeUntilUplink()) {
    if(++unavailableFailures >= 3) { cooldown(); }
  } else { txFailures = capacityFailures = 0; }
  return transaction;
}
uint32_t lwGenerateResetCode() {
  // Public radio entropy API, used only between completed transactions. Assemble
  // unsigned bytes here to avoid signed-overflow behavior in generic helpers.
  uint32_t entropy = 0;
  for(unsigned i = 0; i < 4; ++i) { entropy = (entropy << 8) | gRadio.randomByte(); }
  return 100000UL + entropy % 900000UL;
}
