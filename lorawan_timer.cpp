#include "lorawan_timer.h"
#include <Arduino.h>

bool lwTimerRolloverPending() {
  return millis() >= UINT32_MAX - LW_TRANSACTION_GUARD_MS;
}
