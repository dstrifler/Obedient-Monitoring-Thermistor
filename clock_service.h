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

// RTC runs from XTAL; STIMER and Arduino millis run from HFRC. This is an
// independent reference comparison, unlike comparing millis with STIMER.
static bool clockReadRtcHundredths(uint32_t* value) {
  am_hal_rtc_time_t time = {};
  if(am_hal_rtc_time_get(&time) != 0 || time.ui32ReadError ||
     time.ui32Minute > 59 || time.ui32Second > 59 || time.ui32Hundredths > 99) { return false; }
  *value = (time.ui32Minute * 60 + time.ui32Second) * 100 + time.ui32Hundredths;
  return true;
}

static bool clockVerifyRate() {
  am_hal_rtc_osc_enable();
  uint32_t startRtc;
  if(!clockReadRtcHundredths(&startRtc)) { return false; }
  uint32_t startTicks = am_hal_stimer_counter_get();
  // A 20-second window limits RTC's 10-ms endpoint quantization error.
  // 2000 ppm is a coarse startup sanity limit, not RF timing certification.
  for(uint32_t polls = 0; polls < 30000; ++polls) {
    delay(1);
    uint32_t rtc;
    if(!clockReadRtcHundredths(&rtc)) { return false; }
    uint32_t ticks = am_hal_stimer_counter_get();
    uint32_t hundredths = (rtc + 360000UL - startRtc) % 360000UL;
    if(hundredths >= 2000) {
      if(hundredths > 2100) { return false; }
      uint32_t elapsedTicks = ticks - startTicks;
      uint32_t expected = hundredths * 30000UL;
      int64_t error = static_cast<int64_t>(elapsedTicks) - expected;
      int32_t ppm = static_cast<int32_t>(error * 1000000LL / expected);
      Serial.print(F("[Clock] STIMER vs XTAL RTC error (ppm, coarse): "));
      Serial.println(ppm);
      return ppm >= -2000 && ppm <= 2000;
    }
  }
  Serial.println(F("[Clock] RTC did not advance within the bounded check."));
  return false;
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

  // One-second HFADJ warmup plus four complete four-second correction cycles.
  // This settling margin is followed by measurement, not assumed convergence.
  delay(17000);
  bool configured = clockCheckConfiguration() && clockVerifyRate() && clockCheckConfiguration();
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
