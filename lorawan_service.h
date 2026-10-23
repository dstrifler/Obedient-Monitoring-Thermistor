#ifndef LORAWAN_SERVICE_H
#define LORAWAN_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <RadioLib.h>

static constexpr int16_t LW_ERR_PERSISTENCE = -32000;
static constexpr int16_t LW_ERR_NONCE_EXHAUSTED = -31999;

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

#endif
