#ifndef LORAWAN_TIMER_H
#define LORAWAN_TIMER_H

#include <Arduino.h>
#include <stdint.h>

// RadioLib 7.1.2 uses absolute uint32_t timestamps inside blocking calls.
// Leave ten minutes for a complete US915 Class A transaction, including up to
// 15 NbTrans repetitions and the maximum 16-second RX2 delay, plus PHY/CPU
// margin. Do not start another transaction in this interval. Revisit this
// budget when changing region, modem, or the library's blocking behavior.
static constexpr uint32_t LW_TRANSACTION_GUARD_MS = 10UL * 60UL * 1000UL;
static constexpr int16_t LW_ERR_TIMER_ROLLOVER = -31998;

static bool lwTimerRolloverPending() {
  return millis() >= UINT32_MAX - LW_TRANSACTION_GUARD_MS;
}

#endif
