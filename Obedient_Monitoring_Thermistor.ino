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
static const uint32_t LOOP_SLICE_MS = 50UL;

enum SchedulerState : uint8_t {
  SCHED_WAIT_FOR_JOIN = 0,
  SCHED_WAIT_FOR_REPORT,
  SCHED_SEND_UPLINK,
  SCHED_PROCESS_DOWNLINK
};

static SchedulerState gSchedulerState = SCHED_WAIT_FOR_JOIN;
static uint8_t gPendingDownlinkBuffer[255];
static size_t gPendingDownlinkLen = 0;
static uint32_t gNextSchedulerTickMs = 0;

// ============================================================
// HELPERS
// ============================================================

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
    gLastReportMs = millis() - REPORT_INTERVAL_MS;
    Serial.println(F("[LoRaWAN] Join successful."));
    return true;
  }

  Serial.println(F("[LoRaWAN] Join attempt failed."));
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
    while(true) {
      delay(1000);
    }
  }

  eepromBootstrap();
  Serial.println(F("[Test] Sensor disabled. Sending Hello World every 60 seconds after join."));

  if(!lwBegin()) {
    Serial.println(F("[LoRaWAN] Begin failed."));
    while(true) {
      delay(1000);
    }
  }

  int16_t restoreState = lwRestore();
  Serial.print(F("[LoRaWAN] Restore state: "));
  Serial.println(restoreState);

  gLoRaJoined = false;
  gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
  gLastReportMs = millis() - REPORT_INTERVAL_MS;
  gNextSchedulerTickMs = 0;
  gSchedulerState = SCHED_WAIT_FOR_JOIN;

  Serial.println(F("[LoRaWAN] Join deferred to scheduler."));
}

// ============================================================
// LOOP
// ============================================================
// The test bypasses sensor acquisition and saved application settings.
// RadioLib still owns activation and Class A receive-window timing.

void loop() {
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

      if((now - gLastReportMs) < REPORT_INTERVAL_MS) {
        return;
      }

      gSchedulerState = SCHED_SEND_UPLINK;
      break;

    case SCHED_SEND_UPLINK: {
      const size_t payloadLen = sizeof(HELLO_WORLD_PAYLOAD) - 1;
      size_t maxPayloadBytes = lwMaxPayloadBytes();
      if((maxPayloadBytes == 0) || (payloadLen > maxPayloadBytes)) {
        Serial.println(F("[Payload] Hello World exceeds current payload limit. Retrying after backoff."));
        gLastReportMs = now - REPORT_INTERVAL_MS + REPORT_RETRY_BACKOFF_MS;
        gSchedulerState = SCHED_WAIT_FOR_REPORT;
        return;
      }

      Serial.println(F("[Payload] TEXT: Hello World (11 bytes)"));
      gPendingDownlinkLen = sizeof(gPendingDownlinkBuffer);
      memset(gPendingDownlinkBuffer, 0, gPendingDownlinkLen);

      Serial.println(F("[LoRaWAN] Sending uplink ..."));

      int16_t state = lwSendReceive(
        HELLO_WORLD_PAYLOAD,
        payloadLen,
        gPendingDownlinkBuffer,
        &gPendingDownlinkLen
      );

      if(state < 0) {
        Serial.print(F("[LoRaWAN] Send failed, code "));
        Serial.println(state);

        if(state == LW_ERR_INVALID_ARGUMENT) {
          Serial.println(F("[LoRaWAN] Send aborted: invalid API arguments (payload/downlink buffers)."));
          gLastReportMs = now - REPORT_INTERVAL_MS + REPORT_RETRY_BACKOFF_MS;
          gSchedulerState = SCHED_WAIT_FOR_REPORT;
          return;
        }

        if(state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
          gLoRaJoined = false;
          gLastJoinAttemptMs = millis() - JOIN_RETRY_MS;
          gSchedulerState = SCHED_WAIT_FOR_JOIN;
          Serial.println(F("[LoRaWAN] Session lost. Will retry join."));
          return;
        }

        gLastReportMs = now - REPORT_INTERVAL_MS + REPORT_RETRY_BACKOFF_MS;
        gSchedulerState = SCHED_WAIT_FOR_REPORT;
        return;
      }

      Serial.println(F("[LoRaWAN] Uplink sent successfully."));
      gLastReportMs = now;
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

    default:
      gSchedulerState = SCHED_WAIT_FOR_JOIN;
      break;
  }
}
