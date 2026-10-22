#ifndef _CLOCK_SERVICE_H
#define _CLOCK_SERVICE_H

#include <Arduino.h>
#include "am_mcu_apollo.h"

#include "config.h"

// Apollo3 HFADJ defaults used by AmbiqSuite for a 48 MHz HFRC calibrated
// against the 32.768 kHz crystal: gain 1/2 and a four-second adjustment cycle.
static const uint32_t CLOCK_HFADJ_TARGET_48MHZ = 0x5B8UL;

static bool clockCheckConfiguration() {
  am_hal_clkgen_status_t status;
  uint32_t statusResult = am_hal_clkgen_status_get(&status);

  bool rtcUsesXtal =
    (statusResult == AM_HAL_STATUS_SUCCESS) &&
    (status.eRTCOSC == AM_HAL_CLKGEN_STATUS_RTCOSC_XTAL) &&
    !status.bXtalFailure;
  bool hfadjEnabled =
    (CLKGEN->HFADJ_b.HFADJEN == CLKGEN_HFADJ_HFADJEN_EN);
  bool hfadjTargetCorrect =
    (CLKGEN->HFADJ_b.HFXTADJ == CLOCK_HFADJ_TARGET_48MHZ);
  bool hfadjGainCorrect =
    (CLKGEN->HFADJ_b.HFADJGAIN == CLKGEN_HFADJ_HFADJGAIN_Gain_of_1_in_2);
  bool hfadjIntervalCorrect =
    (CLKGEN->HFADJ_b.HFADJCK == CLKGEN_HFADJ_HFADJCK_4SEC);
  bool stimerUsesHfrcDiv16 =
    (CTIMER->STCFG_b.CLKSEL == CTIMER_STCFG_CLKSEL_HFRC_DIV16);

  Serial.println(F("[Clock] Configuration:"));
  Serial.print(F("[Clock]   RTC reference: "));
  Serial.println(rtcUsesXtal ? F("32.768 kHz XTAL") : F("INVALID"));
  Serial.print(F("[Clock]   HFADJ: "));
  Serial.println(hfadjEnabled ? F("enabled") : F("disabled"));
  Serial.print(F("[Clock]   HFADJ target: 0x"));
  Serial.println(CLKGEN->HFADJ_b.HFXTADJ, HEX);
  Serial.print(F("[Clock]   HFADJ gain: "));
  Serial.println(hfadjGainCorrect ? F("1/2") : F("INVALID"));
  Serial.print(F("[Clock]   HFADJ interval: "));
  Serial.println(hfadjIntervalCorrect ? F("4 seconds") : F("INVALID"));
  Serial.print(F("[Clock]   STIMER source: "));
  Serial.println(stimerUsesHfrcDiv16 ? F("HFRC / 16 (~3 MHz)") : F("INVALID"));

  return rtcUsesXtal &&
         hfadjEnabled &&
         hfadjTargetCorrect &&
         hfadjGainCorrect &&
         hfadjIntervalCorrect &&
         stimerUsesHfrcDiv16;
}

bool clockBegin() {
  Serial.println(F("[Clock] Starting 32.768 kHz crystal ..."));

  // HFADJ is not safe on Apollo3 revisions affected by the pre-B0 crystal
  // glitch erratum. Do not trade inaccurate timing for an unstable clock.
  if(!APOLLO3_B0) {
    Serial.println(F("[Clock] HFADJ requires Apollo3 B0 or newer silicon."));
    return false;
  }

  uint32_t state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_XTAL_START,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    Serial.print(F("[Clock] Crystal start failed, code "));
    Serial.println(state);
    return false;
  }

  state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_RTC_SEL_XTAL,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    Serial.print(F("[Clock] Crystal selection failed, code "));
    Serial.println(state);
    return false;
  }

  // Ambiq requires two seconds after crystal start/selection before checking
  // its status or using it as the HFADJ reference.
  delay(CLOCK_XTAL_STABILIZATION_MS);

  // A null configuration selects AmbiqSuite's Apollo3 defaults: 48 MHz
  // target, gain 1/2, one-second warmup, and a four-second adjustment cycle.
  state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_HFADJ_ENABLE,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    Serial.print(F("[Clock] HFADJ enable failed, code "));
    Serial.println(state);
    return false;
  }

  bool configured = clockCheckConfiguration();
  Serial.println(configured
    ? F("[Clock] Crystal-calibrated system timing ready.")
    : F("[Clock] Configuration verification failed."));
  return configured;
}

void clockLogTimestamp(const __FlashStringHelper* eventName) {
  Serial.print(F("[Timing] "));
  Serial.print(eventName);
  Serial.print(F(": millis="));
  Serial.print(millis());
  Serial.print(F(", stimerTicks="));
  Serial.println(am_hal_stimer_counter_get());
}

#endif
