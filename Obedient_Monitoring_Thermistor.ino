#include <Arduino.h>

#include "config.h"
#include "clock_service.h"
#include "eeprom_service.h"
#include "lorawan_service.h"

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
static uint8_t gMacOnlyAttempts = 0;
static uint32_t gFaultLogMs = 0;
static const uint32_t LOOP_SLICE_MS = 50UL;

enum SchedulerState : uint8_t {
  SCHED_WAIT_FOR_JOIN = 0,
  SCHED_WAIT_FOR_REPORT,
  SCHED_SEND_UPLINK,
  SCHED_PROCESS_DOWNLINK,
  SCHED_FAULT
};

static SchedulerState gSchedulerState = SCHED_WAIT_FOR_JOIN;
static uint8_t gPendingDownlinkBuffer[255];
static size_t gPendingDownlinkLen = 0;
static uint32_t gNextSchedulerTickMs = 0;

// ============================================================
// HELPERS
// ============================================================

static void appFault(const __FlashStringHelper* reason) {
  Serial.print(F("[Fault] ")); Serial.println(reason);
  gSchedulerState = SCHED_FAULT;
  gFaultLogMs = millis();
}

static void scheduleReportRetry(uint32_t waitMs) {
  gLastReportMs = millis();
  // Signed deadline arithmetic requires durations below half the timer range.
  gReportWaitMs = waitMs > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : waitMs;
  gSchedulerState = SCHED_WAIT_FOR_REPORT;
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
      return true;
    default: return false;
  }
}

static void printDownlinkPayload(const uint8_t* buffer, size_t len) {
  Serial.print(F("[Downlink] Length: "));
  Serial.println(len);

  Serial.print(F("[Downlink] HEX: "));
  for(size_t i = 0; i < len; i++) {
    if(buffer[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(buffer[i], HEX);
  }
  Serial.println();
}

static bool ensureJoined() {
  gLoRaJoined = lwIsActivated();
  if(gLoRaJoined) {
    return true;
  }

  Serial.println(F("[LoRaWAN] Not joined. Trying to join..."));

  gLoRaJoined = lwActivate();
  gLoRaJoined = gLoRaJoined && lwIsActivated();

  if(gLoRaJoined) {
    gReportWaitMs = REPORT_INTERVAL_MS;
    gLastReportMs = millis() - REPORT_INTERVAL_MS;
    Serial.println(F("[LoRaWAN] Join successful."));
    return true;
  }

  Serial.print(F("[LoRaWAN] Join attempt failed, code "));
  Serial.println(lwLastActivationState());
  if(!lwPersistenceHealthy() || permanentLoRaError(lwLastActivationState())) {
    appFault(F("Join stopped: resolve persistence/configuration error before reset."));
  }
  return false;
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(2000);

  Serial.println();
  Serial.println(F("Garage Hello World Test Starting..."));

  // Calibrate the Apollo3 HFRC before initializing the SX1262 or RadioLib.
  // Abort startup if timing cannot be verified: unreliable timing can miss
  // the tightly scheduled LoRaWAN receive windows.
  if(!clockBegin()) {
    Serial.println(F("[Clock] Fatal timing initialization failure."));
    appFault(F("Clock startup verification failed; radio remains disabled."));
    return;
  }

  if(!eepromBootstrap()) {
    appFault(F("EEPROM initialization failed."));
    return;
  }
  Serial.println(F("[Test] Sensor disabled. Sending Hello World every 60 seconds after join."));

  bool radioReady = false;
  for(uint8_t attempt = 0; attempt < 3 && !radioReady; ++attempt) {
    radioReady = lwBegin();
    if(!radioReady) { delay(1000UL << attempt); }
  }
  if(!radioReady) {
    appFault(F("Radio startup failed after three attempts. Check wiring/configuration."));
    return;
  }

  int16_t restoreState = lwRestore();
  Serial.print(F("[LoRaWAN] Restore state: "));
  Serial.println(restoreState);

  if(!lwPersistenceHealthy()) {
    appFault(F("Persistence restore failed; saved history retained."));
    return;
  }

  gLoRaJoined = false;
  gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
  gLastReportMs = millis() - REPORT_INTERVAL_MS;
  gNextSchedulerTickMs = millis();
  gSchedulerState = SCHED_WAIT_FOR_JOIN;

  Serial.println(F("[LoRaWAN] Join deferred to scheduler."));
}

// ============================================================
// LOOP
// ============================================================
// The test bypasses sensor acquisition and saved application settings.
// RadioLib still owns activation and Class A receive-window timing.

void loop() {
  delay(1);  // Yield to the RTOS between scheduler checks.
  uint32_t now = millis();
  if((int32_t)(now - gNextSchedulerTickMs) < 0) {
    return;
  }
  gNextSchedulerTickMs = now + LOOP_SLICE_MS;

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

      gSchedulerState = SCHED_WAIT_FOR_REPORT;
      break;

    case SCHED_WAIT_FOR_REPORT:
      gLoRaJoined = lwIsActivated();
      if(!gLoRaJoined) {
        gSchedulerState = SCHED_WAIT_FOR_JOIN;
        return;
      }

      if((now - gLastReportMs) < gReportWaitMs) {
        return;
      }

      gSchedulerState = SCHED_SEND_UPLINK;
      break;

    case SCHED_SEND_UPLINK: {
      uint32_t waitMs = lwTimeUntilUplink();
      if(waitMs > 0) { scheduleReportRetry(waitMs); return; }
      const size_t payloadLen = sizeof(HELLO_WORLD_PAYLOAD) - 1;
      size_t maxPayloadBytes = lwMaxPayloadBytes();
      bool macOnly = payloadLen > maxPayloadBytes;
      if(macOnly && ++gMacOnlyAttempts > 3) {
        appFault(F("Payload still unavailable after three MAC-only transactions."));
        return;
      }
      Serial.println(macOnly ? F("[Payload] MAC-only transaction; text remains pending.") :
        F("[Payload] TEXT: Hello World (11 bytes)"));
      gPendingDownlinkLen = 0;
      Serial.println(F("[LoRaWAN] Sending uplink ..."));

      int16_t state = lwSendReceive(
        HELLO_WORLD_PAYLOAD,
        macOnly ? 0 : payloadLen,
        gPendingDownlinkBuffer,
        &gPendingDownlinkLen
      );

      if(state < 0) {
        Serial.print(F("[LoRaWAN] Send failed, code "));
        Serial.println(state);

        if(permanentLoRaError(state)) {
          appFault(F("Permanent uplink configuration/API error."));
          return;
        }

        if(state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
          gLoRaJoined = false;
          gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
          gSchedulerState = SCHED_WAIT_FOR_JOIN;
          Serial.println(F("[LoRaWAN] Session lost. Will retry join."));
          return;
        }

        if(state == RADIOLIB_ERR_UPLINK_UNAVAILABLE) {
          waitMs = lwTimeUntilUplink();
          scheduleReportRetry(waitMs > REPORT_RETRY_BACKOFF_MS ? waitMs : REPORT_RETRY_BACKOFF_MS);
        } else if(state == RADIOLIB_ERR_PACKET_TOO_LONG || state == RADIOLIB_ERR_DWELL_TIME_EXCEEDED) {
          // Re-evaluate the live limit next time; do not force a radio reset.
          scheduleReportRetry(REPORT_INTERVAL_MS);
        } else {
          // A receive-side failure can follow an actual transmission. Avoid
          // generating a new frame every five seconds during a lasting fault.
          scheduleReportRetry(gRetryBackoffMs);
          gRetryBackoffMs = gRetryBackoffMs >= 150000UL ? 300000UL : gRetryBackoffMs * 2;
        }
        return;
      }

      Serial.print(F("[LoRaWAN] Local transaction complete, code "));
      Serial.println(state);
      Serial.println(F("[LoRaWAN] Network receipt requires console verification."));
      gRetryBackoffMs = REPORT_RETRY_BACKOFF_MS;
      if(macOnly) {
        gLastReportMs = millis();
        gReportWaitMs = REPORT_RETRY_BACKOFF_MS;
      } else {
        gMacOnlyAttempts = 0;
        gLastReportMs = now;
        gReportWaitMs = REPORT_INTERVAL_MS;
      }
      gSchedulerState = SCHED_PROCESS_DOWNLINK;
      break;
    }

    case SCHED_PROCESS_DOWNLINK:
      if(gPendingDownlinkLen > 0) {
        printDownlinkPayload(gPendingDownlinkBuffer, gPendingDownlinkLen);

        Serial.println(F("[Test] Application commands ignored; payload and interval are fixed."));
      } else {
        Serial.println(F("[Downlink] No app downlink received."));
      }

      gSchedulerState = SCHED_WAIT_FOR_REPORT;
      break;

    case SCHED_FAULT:
      if(now - gFaultLogMs >= 30000UL) {
        Serial.println(F("[Fault] Radio work paused; resolve the logged cause before restarting."));
        gFaultLogMs = now;
      }
      delay(50);
      return;

    default:
      gSchedulerState = SCHED_WAIT_FOR_JOIN;
      break;
  }
}
