#ifndef _CLOCK_SERVICE_H
#define _CLOCK_SERVICE_H

#include <Arduino.h>
#include "am_mcu_apollo.h"

#include "config.h"
#include "log_service.h"

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

  bool configured = rtcUsesXtal && hfadjEnabled && hfadjTargetCorrect &&
    hfadjGainCorrect && hfadjIntervalCorrect && stimerUsesHfrcDiv16;
  if(!configured) {
    LOG_ERROR("Clock", "Configuration invalid: rtc_xtal=%u hfadj=%u target_ok=%u gain_ok=%u interval_ok=%u stimer_ok=%u",
      unsigned(rtcUsesXtal), unsigned(hfadjEnabled), unsigned(hfadjTargetCorrect),
      unsigned(hfadjGainCorrect), unsigned(hfadjIntervalCorrect), unsigned(stimerUsesHfrcDiv16));
  } else {
    LOG_DEBUG("Clock", "RTC=XTAL HFADJ target=0x%lx, gain=1/2, interval=4s; STIMER=HFRC/16",
      static_cast<unsigned long>(CLKGEN->HFADJ_b.HFXTADJ));
  }
  return configured;
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

struct ClockRateSample {
  uint32_t rtcHundredths;
  uint32_t stimerTicks;
};

static bool clockReadRateSample(ClockRateSample* sample) {
  for(uint8_t attempt = 0; attempt < 5; ++attempt) {
    uint32_t before = am_hal_stimer_counter_get();
    uint32_t rtc;
    bool valid = clockReadRtcHundredths(&rtc);
    uint32_t after = am_hal_stimer_counter_get();
    uint32_t span = after - before;
    // Bracket the RTC read with STIMER samples. Reject a preempted read;
    // midpoint uncertainty is at most 1 ms, below RTC's 10-ms resolution.
    if(valid && span <= 6000UL) {
      sample->rtcHundredths = rtc;
      sample->stimerTicks = before + span / 2;
      return true;
    }
    delay(1);
  }
  return false;
}

static bool clockVerifyRateWindow() {
  ClockRateSample start;
  if(!clockReadRateSample(&start)) { return false; }
  uint32_t startedMs = millis();
  // A 20-second window limits RTC's 10-ms endpoint quantization error.
  // Both elapsed-time and poll-count bounds protect against a stopped RTC.
  for(uint32_t polls = 0; polls < 30000 && (millis() - startedMs) < 30000UL; ++polls) {
    delay(1);
    logPump();
    ClockRateSample end;
    if(!clockReadRateSample(&end)) { continue; }
    uint32_t hundredths = (end.rtcHundredths + 360000UL - start.rtcHundredths) % 360000UL;
    if(hundredths >= 2000) {
      if(hundredths > 2100) { return false; }
      uint32_t elapsedTicks = end.stimerTicks - start.stimerTicks;
      uint32_t expected = hundredths * 30000UL;
      int64_t error = static_cast<int64_t>(elapsedTicks) - expected;
      int32_t ppm = static_cast<int32_t>(error * 1000000LL / expected);
      LOG_INFO("Clock", "STIMER/XTAL RTC error=%ld ppm (coarse)", static_cast<long>(ppm));
      return ppm >= -2000 && ppm <= 2000;
    }
  }
  LOG_WARN("Clock", "RTC did not advance within the bounded check.");
  return false;
}

static bool clockVerifyRate() {
  am_hal_rtc_osc_enable();
  for(uint8_t attempt = 0; attempt < 3; ++attempt) {
    if(clockVerifyRateWindow()) { return true; }
    LOG_WARN("Clock", "Measurement invalid or out of tolerance; retrying within startup budget.");
    delay(10);
  }
  return false;
}

bool clockBegin() {
  LOG_INFO("Clock", "Starting 32.768 kHz crystal ...");

  // HFADJ is not safe on Apollo3 revisions affected by the pre-B0 crystal
  // glitch erratum. Do not trade inaccurate timing for an unstable clock.
  if(!APOLLO3_B0) {
    LOG_ERROR("Clock", "HFADJ requires Apollo3 B0 or newer silicon.");
    return false;
  }

  uint32_t state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_XTAL_START,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    LOG_ERROR("Clock", "Crystal start failed: code=%lu", static_cast<unsigned long>(state));
    return false;
  }

  state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_RTC_SEL_XTAL,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    LOG_ERROR("Clock", "Crystal selection failed: code=%lu", static_cast<unsigned long>(state));
    return false;
  }

  // Ambiq requires two seconds after crystal start/selection before checking
  // its status or using it as the HFADJ reference.
  logServiceDelay(CLOCK_XTAL_STABILIZATION_MS);

  // A null configuration selects AmbiqSuite's Apollo3 defaults: 48 MHz
  // target, gain 1/2, one-second warmup, and a four-second adjustment cycle.
  state = am_hal_clkgen_control(
    AM_HAL_CLKGEN_CONTROL_HFADJ_ENABLE,
    nullptr
  );
  if(state != AM_HAL_STATUS_SUCCESS) {
    LOG_ERROR("Clock", "HFADJ enable failed: code=%lu", static_cast<unsigned long>(state));
    return false;
  }

  // One-second HFADJ warmup plus four complete four-second correction cycles.
  // This settling margin is followed by measurement, not assumed convergence.
  logServiceDelay(17000);
  bool configured = clockCheckConfiguration() && clockVerifyRate() && clockCheckConfiguration();
  if(configured) { LOG_INFO("Clock", "Crystal-calibrated system timing verified."); }
  else { LOG_ERROR("Clock", "Timing verification failed."); }
  return configured;
}


#endif
