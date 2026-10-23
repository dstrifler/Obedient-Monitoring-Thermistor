#include "app_service.h"
#include <Arduino.h>

#include "config.h"
#include "log_service.h"
#include "clock_service.h"
#include "eeprom_service.h"
#include "lorawan_service.h"
#include "lorawan_timer.h"
#include "sensor_service.h"

// ============================================================
// APP STATE
// ============================================================

static uint32_t gLastReportMs = 0;
static uint32_t gLastJoinAttemptMs = 0;
static bool gLoRaJoined = false;
// Fixed test cadence, independent of saved settings and sensor defaults.
static constexpr uint32_t REPORT_INTERVAL_MS = 60UL * 1000UL;
// Send only these 11 ASCII bytes, without the trailing NUL.
static const uint8_t HELLO_WORLD_PAYLOAD[] = "Hello World";

static const uint32_t JOIN_RETRY_MS = 60000UL;
static const uint32_t REPORT_RETRY_BACKOFF_MS = 5000UL;
static uint32_t gReportWaitMs = REPORT_INTERVAL_MS;
static uint32_t gRetryBackoffMs = REPORT_RETRY_BACKOFF_MS;
static uint8_t gUnavailableAttempts = 0;
static uint8_t gJoinHardwareFailures = 0;
static uint8_t gUplinkHardwareFailures = 0;
static uint8_t gCapacityFailures = 0;
static uint16_t gCapacityRatesTried = 0;
static uint8_t gRecoveryAttempts = 0;
static uint32_t gRecoveryStartedMs = 0;
static uint32_t gRolloverWaitStartedMs = 0;
static uint32_t gLastSchedulerTimeMs = 0;
// Allow any abandoned Class A windows to expire before resetting radio state.
static const uint32_t RADIO_RECOVERY_COOLDOWN_MS = 60000UL;
static uint32_t gFaultLogMs = 0;
static const uint32_t LOOP_SLICE_MS = 50UL;

enum SchedulerState : uint8_t {
  SCHED_WAIT_FOR_JOIN = 0,
  SCHED_WAIT_FOR_REPORT,
  SCHED_SEND_UPLINK,
  SCHED_PROCESS_DOWNLINK,
  SCHED_RECOVER_RADIO,
  SCHED_WAIT_TIMER_ROLLOVER,
  SCHED_FAULT
};

static SchedulerState gSchedulerState = SCHED_WAIT_FOR_JOIN;
static uint8_t gPendingDownlinkBuffer[255];
static size_t gPendingDownlinkLen = 0;
static uint32_t gNextSchedulerTickMs = 0;

// State transitions are emitted only at TRACE, never once per idle tick.
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_TRACE
static const char* schedulerStateName(SchedulerState state) {
  switch(state) {
    case SCHED_WAIT_FOR_JOIN: return "WAIT_FOR_JOIN";
    case SCHED_WAIT_FOR_REPORT: return "WAIT_FOR_REPORT";
    case SCHED_SEND_UPLINK: return "SEND_UPLINK";
    case SCHED_PROCESS_DOWNLINK: return "PROCESS_DOWNLINK";
    case SCHED_RECOVER_RADIO: return "RECOVER_RADIO";
    case SCHED_WAIT_TIMER_ROLLOVER: return "WAIT_TIMER_ROLLOVER";
    case SCHED_FAULT: return "FAULT";
    default: return "UNKNOWN";
  }
}
#endif
static void setSchedulerState(SchedulerState next) {
  if(next != gSchedulerState) {
    LOG_TRACE("Scheduler", "%s -> %s", schedulerStateName(gSchedulerState), schedulerStateName(next));
  }
  gSchedulerState = next;
}

// ============================================================
// HELPERS
// ============================================================

static void appFault(const char* reason) {
  LOG_ERROR("App", "%s", reason);
  (void)reason;
  setSchedulerState(SCHED_FAULT);
  gFaultLogMs = millis();
}

static void scheduleReportRetry(uint32_t waitMs) {
  gLastReportMs = millis();
  // Library deferrals are validated and sliced by deferForAirtime(). Other
  // callers supply the application's bounded retry/cadence intervals.
  gReportWaitMs = waitMs;
  setSchedulerState(SCHED_WAIT_FOR_REPORT);
}

static void scheduleRadioRecovery() {
  LOG_WARN("LoRaWAN", "Cooling down before radio recovery.");
  gLoRaJoined = false;
  gPendingDownlinkLen = 0;
  gRecoveryAttempts = 0;
  gJoinHardwareFailures = 0;
  gUplinkHardwareFailures = 0;
  gRecoveryStartedMs = millis();
  setSchedulerState(SCHED_RECOVER_RADIO);
}

static void scheduleTimerRolloverWait() {
  const uint32_t started = millis();
  if(started < UINT32_MAX - LW_TRANSACTION_GUARD_MS) {
    // The boundary may have passed between the wrapper's guard and this call.
    scheduleRadioRecovery();
    return;
  }
  LOG_INFO("LoRaWAN", "Pausing radio work until timer rollover.");
  gLoRaJoined = false;
  gPendingDownlinkLen = 0;
  gRolloverWaitStartedMs = started;
  setSchedulerState(SCHED_WAIT_TIMER_ROLLOVER);
}

static bool deferForAirtime(uint32_t waitMs) {
  if(waitMs == 0) { return false; }
  if(waitMs > 0x7FFFFFFFUL) {
    LOG_WARN("LoRaWAN", "Invalid airtime wait exceeds the supported timer range.");
    scheduleRadioRecovery();
  } else {
    gUnavailableAttempts = 0;
    // Re-query eligibility at least once a minute; never force RF before the
    // library permits it, even when its legitimate deferral is much longer.
    scheduleReportRetry(waitMs > REPORT_INTERVAL_MS ? REPORT_INTERVAL_MS : waitMs);
  }
  return true;
}

static bool hardwareLoRaError(int16_t state) {
  switch(state) {
    case RADIOLIB_ERR_TX_TIMEOUT:
    case RADIOLIB_ERR_RX_TIMEOUT:
    case RADIOLIB_ERR_SPI_WRITE_FAILED:
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:
    case RADIOLIB_ERR_SPI_CMD_INVALID:
    case RADIOLIB_ERR_SPI_CMD_FAILED:
    case RADIOLIB_ERR_CHIP_NOT_FOUND:
      return true;
    default: return false;
  }
}

static bool permanentLoRaError(int16_t state) {
  switch(state) {
    case LW_ERR_INVALID_ARGUMENT:
    case RADIOLIB_ERR_NULL_POINTER:
    case RADIOLIB_ERR_INVALID_PORT:
    case RADIOLIB_ERR_INVALID_REVISION:
    case RADIOLIB_ERR_INVALID_MODE:
    case RADIOLIB_ERR_INVALID_DATA_RATE:
    case RADIOLIB_ERR_INVALID_FREQUENCY:
    case LW_ERR_PERSISTENCE:
    case LW_ERR_NONCE_EXHAUSTED:
    case LW_ERR_RADIO_GPIO:
      return true;
    default: return false;
  }
}

static bool ensureJoined() {
  gLoRaJoined = lwIsActivated();
  if(gLoRaJoined) {
    return true;
  }

  LOG_DEBUG("LoRaWAN", "Not joined. Trying to join...");

  gLoRaJoined = lwActivate();
  gLoRaJoined = gLoRaJoined && lwIsActivated();

  if(gLoRaJoined) {
    gUnavailableAttempts = 0;
    gJoinHardwareFailures = 0;
    gUplinkHardwareFailures = 0;
    gCapacityFailures = 0;
    gCapacityRatesTried = 0;
    gRetryBackoffMs = REPORT_RETRY_BACKOFF_MS;
    gReportWaitMs = REPORT_INTERVAL_MS;
    // Start the first interval after activation has returned, including after
    // recovery. Keep servicing the scheduler and serial output while waiting.
    gLastReportMs = millis();
    LOG_INFO("LoRaWAN", "Joined; first uplink scheduled in 60 seconds.");
    return true;
  }

  LOG_DEBUG("LoRaWAN", "Join attempt returned code=%d", int(lwLastActivationState()));
  if(!lwPersistenceHealthy() || permanentLoRaError(lwLastActivationState())) {
    appFault("Join stopped: resolve persistence/configuration error before reset.");
  } else if(lwLastActivationState() == LW_ERR_TIMER_ROLLOVER) {
    scheduleTimerRolloverWait();
  } else if(hardwareLoRaError(lwLastActivationState())) {
    if(++gJoinHardwareFailures >= 3) { scheduleRadioRecovery(); }
  } else {
    gJoinHardwareFailures = 0;  // No JoinAccept is a network retry, not a radio fault.
  }
  return false;
}

// ============================================================
// SETUP
// ============================================================

void appSetup() {
  logBegin(SERIAL_BAUD_RATE);
  logServiceDelay(2000);

  LOG_INFO("App", "Garage Hello World Test Starting...");

  // Calibrate the Apollo3 HFRC before initializing the SX1262 or RadioLib.
  // Abort startup if timing cannot be verified: unreliable timing can miss
  // the tightly scheduled LoRaWAN receive windows.
  if(!clockBegin()) {
    LOG_ERROR("Clock", "Fatal timing initialization failure.");
    appFault("Clock startup verification failed; radio remains disabled.");
    return;
  }

  if(!eepromBootstrap()) {
    appFault("EEPROM initialization failed.");
    return;
  }
  LOG_INFO("Test", "Hello World starts 60 seconds after join, then repeats every 60 seconds.");
  sensorStart();

  bool radioReady = false;
  for(uint8_t attempt = 0; attempt < 3 && !radioReady; ++attempt) {
    radioReady = lwBegin();
    if(!radioReady) { logServiceDelay(1000UL << attempt); }
  }
  if(!radioReady) {
    appFault("Radio startup failed after three attempts. Check wiring/configuration.");
    return;
  }

  int16_t restoreState = lwRestore();
  LOG_DEBUG("LoRaWAN", "Restore state=%d", int(restoreState));
  (void)restoreState;

  if(!lwPersistenceHealthy()) {
    appFault("Persistence restore failed; saved history retained.");
    return;
  }

  gLoRaJoined = false;
  gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
  gLastReportMs = millis() - REPORT_INTERVAL_MS;
  gNextSchedulerTickMs = millis();
  gLastSchedulerTimeMs = gNextSchedulerTickMs;
  setSchedulerState(SCHED_WAIT_FOR_JOIN);

  LOG_DEBUG("LoRaWAN", "Join deferred to scheduler.");
}

// ============================================================
// LOOP
// ============================================================
// Sensor diagnostics run alongside the fixed radio test; saved app settings
// and environmental payload encoding remain bypassed.
// RadioLib still owns activation and Class A receive-window timing.

void appLoop() {
  logPump();
  uint32_t now = millis();
  int32_t waitMs = (int32_t)(gNextSchedulerTickMs - now);
  // A long execution gap can alias an overdue deadline into a positive wait.
  // Only a deadline within our normal slice may sleep; otherwise resync below.
  if(waitMs > 0 && uint32_t(waitMs) <= LOOP_SLICE_MS) {
    delay((uint32_t)waitMs);  // Yield until the next application scheduler tick.
    now = millis();
    if((int32_t)(now - gNextSchedulerTickMs) < 0) {
      return;
    }
  }
  gNextSchedulerTickMs = now + LOOP_SLICE_MS;
  sensorPoll();  // Main thread only, outside RadioLib's Class A transactions.
  now = millis();

  if(gSchedulerState != SCHED_FAULT && gSchedulerState != SCHED_WAIT_TIMER_ROLLOVER) {
    if(now < gLastSchedulerTimeMs) {
      // Also discard old absolute library timestamps if the loop was suspended
      // across the boundary and missed the pre-wrap pause.
      scheduleRadioRecovery();
    } else if(lwTimerRolloverPending()) {
      scheduleTimerRolloverWait();
    }
  }
  gLastSchedulerTimeMs = now;

  switch(gSchedulerState) {
    case SCHED_WAIT_FOR_JOIN:
      gLoRaJoined = lwIsActivated();
      if(!gLoRaJoined) {
        if((now - gLastJoinAttemptMs) >= JOIN_RETRY_MS) {
          gLastJoinAttemptMs = now;
          gLoRaJoined = ensureJoined();
        }
        return;
      }

      setSchedulerState(SCHED_WAIT_FOR_REPORT);
      break;

    case SCHED_WAIT_FOR_REPORT:
      gLoRaJoined = lwIsActivated();
      if(!gLoRaJoined) {
        setSchedulerState(SCHED_WAIT_FOR_JOIN);
        return;
      }

      if((now - gLastReportMs) < gReportWaitMs) {
        return;
      }

      setSchedulerState(SCHED_SEND_UPLINK);
      break;

    case SCHED_SEND_UPLINK: {
      uint32_t waitMs = lwTimeUntilUplink();
      if(deferForAirtime(waitMs)) { return; }
      const size_t payloadLen = sizeof(HELLO_WORLD_PAYLOAD) - 1;
      size_t maxPayloadBytes = lwMaxPayloadBytes();
      bool macOnly = payloadLen > maxPayloadBytes;
      if(macOnly) { LOG_INFO("Payload", "MAC-only transaction; Hello World remains pending."); }
      else { LOG_DEBUG("Payload", "Hello World: 11 bytes."); }
      gPendingDownlinkLen = 0;
      // Capacity queries configure the PHY. Log the existing result so the
      // output level cannot add radio operations or consume SPI failures.
      LOG_DEBUG("LoRaWAN", "Uplink preflight: max_payload=%u bytes=%u",
        unsigned(maxPayloadBytes), unsigned(macOnly ? 0 : payloadLen));

      int16_t state = lwSendReceive(
        HELLO_WORLD_PAYLOAD,
        macOnly ? 0 : payloadLen,
        gPendingDownlinkBuffer,
        &gPendingDownlinkLen
      );

      if(state < 0) {
        LOG_WARN("LoRaWAN", "Uplink failed: code=%d", int(state));
        bool capacityError = state == RADIOLIB_ERR_PACKET_TOO_LONG || state == RADIOLIB_ERR_DWELL_TIME_EXCEEDED;
        if(!capacityError) { gCapacityFailures = 0; }
        if(!hardwareLoRaError(state)) { gUplinkHardwareFailures = 0; }

        if(permanentLoRaError(state)) {
          appFault("Permanent uplink configuration/API error.");
          return;
        }

        if(state == LW_ERR_TIMER_ROLLOVER) {
          scheduleTimerRolloverWait();
          return;
        }

        if(state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
          gLoRaJoined = false;
          gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
          setSchedulerState(SCHED_WAIT_FOR_JOIN);
          LOG_WARN("LoRaWAN", "Session lost. Will retry join.");
          return;
        }

        if(state == RADIOLIB_ERR_UPLINK_UNAVAILABLE) {
          waitMs = lwTimeUntilUplink();
          if(deferForAirtime(waitMs)) { return; }
          if(++gUnavailableAttempts >= 3) { scheduleRadioRecovery(); return; }
          scheduleReportRetry(REPORT_RETRY_BACKOFF_MS);
        } else if(capacityError) {
          gUnavailableAttempts = 0;
          if(++gCapacityFailures >= 3) {
            gCapacityFailures = 0;
            if(!lwTryCapacityDataRate(payloadLen, gCapacityRatesTried)) {
              appFault("No permitted data rate restores payload capacity. Check network/data-rate configuration.");
              return;
            }
          }
          scheduleReportRetry(REPORT_INTERVAL_MS);
        } else {
          gUnavailableAttempts = 0;
          if(hardwareLoRaError(state) && ++gUplinkHardwareFailures >= 3) {
            scheduleRadioRecovery();
            return;
          }
          // A receive-side failure can follow an actual transmission. Avoid
          // generating a new frame every five seconds during a lasting fault.
          scheduleReportRetry(gRetryBackoffMs);
          gRetryBackoffMs = gRetryBackoffMs >= 150000UL ? 300000UL : gRetryBackoffMs * 2;
        }
        return;
      }

      LOG_INFO("LoRaWAN", "Uplink transaction complete: code=%d; verify receipt in network console.", int(state));
      gRetryBackoffMs = REPORT_RETRY_BACKOFF_MS;
      gUnavailableAttempts = 0;
      gUplinkHardwareFailures = 0;
      gCapacityFailures = 0;
      gCapacityRatesTried = 0;
      // Persistent MAC responses may need multiple successful transactions.
      // Keep the text pending and use the normal cadence for maintenance too.
      gLastReportMs = now;
      gReportWaitMs = REPORT_INTERVAL_MS;
      setSchedulerState(SCHED_PROCESS_DOWNLINK);
      break;
    }

    case SCHED_PROCESS_DOWNLINK:
      if(gPendingDownlinkLen > 0) {
        LOG_INFO("Downlink", "Received %u application bytes; commands ignored for fixed Hello World test.", unsigned(gPendingDownlinkLen));
        LOG_TRACE_HEX("Downlink", gPendingDownlinkBuffer, gPendingDownlinkLen);
      } else {
        LOG_DEBUG("Downlink", "No app downlink received.");
      }

      setSchedulerState(SCHED_WAIT_FOR_REPORT);
      break;

    case SCHED_WAIT_TIMER_ROLLOVER:
      if(now < gRolloverWaitStartedMs) { scheduleRadioRecovery(); }
      return;

    case SCHED_RECOVER_RADIO:
      if(now - gRecoveryStartedMs < RADIO_RECOVERY_COOLDOWN_MS) { return; }
      if(lwRecover()) {
        gUnavailableAttempts = 0;
        gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
        setSchedulerState(SCHED_WAIT_FOR_JOIN);
        LOG_INFO("LoRaWAN", "Radio recovered; fresh OTAA join pending.");
      } else if(!lwPersistenceHealthy() || ++gRecoveryAttempts >= 3) {
        appFault("Radio recovery failed; resolve the logged radio/persistence error.");
      } else {
        gRecoveryStartedMs = millis();  // Bound recovery initialization attempts.
      }
      return;

    case SCHED_FAULT:
      if(now - gFaultLogMs >= 30000UL) {
        LOG_ERROR("Fault", "Radio work paused; resolve the logged cause before restarting.");
        gFaultLogMs = now;
      }
      delay(50);
      return;

    default:
      setSchedulerState(SCHED_WAIT_FOR_JOIN);
      break;
  }
}
