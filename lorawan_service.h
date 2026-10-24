#ifndef LORAWAN_SERVICE_H
#define LORAWAN_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <RadioLib.h>

static constexpr int16_t LW_ERR_PERSISTENCE = -32000;
static constexpr int16_t LW_ERR_NONCE_EXHAUSTED = -31999;
static constexpr int16_t LW_ERR_RADIO_GPIO = -31997;

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

bool lwBegin();
bool lwIsActivated();
int16_t lwRestore();
bool lwRecover();
bool lwActivate();
int16_t lwLastActivationState();
bool lwPersistenceHealthy();
int16_t lwSendReceive(const uint8_t* dataUp, size_t lenUp, uint8_t* dataDown, size_t* lenDown);
size_t lwMaxPayloadBytes();
uint32_t lwTimeUntilUplink();
bool lwTryCapacityDataRate(size_t requiredBytes, uint16_t& triedRates);

// Main-thread radio lifecycle; application delivery policy lives in scheduler.
enum class RadioState : uint8_t { Stopped, Initializing, Joining, Ready, Cooldown, Rollover, Fault };
struct RadioTransaction {
  int16_t result;
  bool applicationSent;
  bool macOnly;
  size_t downlinkSize;
  uint8_t downlink[255];
};
void lwServiceStart();
void lwServicePoll();
void lwServiceStop();
RadioState lwServiceState();
int16_t lwServiceError();
bool lwServiceReady();
uint32_t lwServiceWait();
RadioTransaction lwTransmit(const uint8_t* payload, size_t size);
uint32_t lwGenerateResetCode();

#endif
