#include <Arduino.h>

#include "config.h"
#include "eeprom_service.h"
#include "settings_service.h"
#include "sensor_service.h"
#include "payload_service.h"
#include "lorawan_service.h"

// ============================================================
// APP STATE
// ============================================================

static uint32_t gLastReportMs = 0;
static uint32_t gLastJoinAttemptMs = 0;
static bool gLoRaJoined = false;
static uint32_t gPeriodicitySeconds = DEFAULT_UPLINK_INTERVAL_SECONDS;
static uint32_t gPeriodicityMs = DEFAULT_UPLINK_INTERVAL_SECONDS * 1000UL;

static const uint32_t JOIN_RETRY_MS = 60000UL;
static const uint32_t LOOP_SLICE_MS = 50UL;

enum SchedulerState : uint8_t {
  SCHED_WAIT_FOR_JOIN = 0,
  SCHED_WAIT_FOR_REPORT,
  SCHED_TRIGGER_SENSOR,
  SCHED_FETCH_SENSOR,
  SCHED_BUILD_PAYLOAD,
  SCHED_SEND_UPLINK,
  SCHED_PROCESS_DOWNLINK
};

static SchedulerState gSchedulerState = SCHED_WAIT_FOR_JOIN;
static SensorData gPendingSensorData;
static uint8_t gPendingUplinkBuffer[255];
static size_t gPendingUplinkLen = 0;
static uint8_t gPendingDownlinkBuffer[255];
static size_t gPendingDownlinkLen = 0;
static uint32_t gNextSchedulerTickMs = 0;
static uint32_t gSensorTriggerMs = 0;

// ============================================================
// HELPERS
// ============================================================

static void printCurrentSettings() {
  AppSettings* s = settingsGet();

  Serial.println(F("---- Current Settings ----"));

  Serial.print(F("Temp enabled: "));
  Serial.println(s->tempEnabled ? F("ON") : F("OFF"));

  Serial.print(F("Humidity enabled: "));
  Serial.println(s->humidityEnabled ? F("ON") : F("OFF"));

  Serial.print(F("Pressure enabled: "));
  Serial.println(s->pressureEnabled ? F("ON") : F("OFF"));

  Serial.print(F("Gas enabled: "));
  Serial.println(s->gasEnabled ? F("ON") : F("OFF"));

  Serial.print(F("Temp unit: "));
  Serial.println((s->tempUnit == TEMP_UNIT_F) ? F("F") : F("C"));

  Serial.print(F("Pressure unit: "));
  switch(s->pressureUnit) {
    case PRESSURE_UNIT_HPA:
      Serial.println(F("hPa"));
      break;
    case PRESSURE_UNIT_INHG:
      Serial.println(F("inHg"));
      break;
    case PRESSURE_UNIT_PA:
    default:
      Serial.println(F("Pa"));
      break;
  }

  Serial.print(F("Payload mode: "));
  switch(s->payloadMode) {
    case PAYLOAD_MODE_COMPACT_JSON:
      Serial.println(F("Compact JSON"));
      break;
    case PAYLOAD_MODE_VERBOSE_DEBUG:
      Serial.println(F("Verbose / Debug"));
      break;
    case PAYLOAD_MODE_COMPACT_BIN:
    default:
      Serial.println(F("Compact Binary"));
      break;
  }

  Serial.print(F("Report interval (min): "));
  Serial.println(s->reportIntervalMin);

  Serial.println(F("--------------------------"));
}

static void syncPeriodicityFromSettings() {
  AppSettings* s = settingsGet();
  gPeriodicitySeconds = (uint32_t)s->reportIntervalMin * 60UL;
  gPeriodicityMs = gPeriodicitySeconds * 1000UL;
}

static bool applyDownlinkCommand(const DownlinkCommand& cmd) {
  bool changed = false;

  switch(cmd.type) {
    case CMD_SET_TEMP_ENABLED:
      changed = settingsSetTempEnabled(cmd.value != 0);
      break;

    case CMD_SET_HUMIDITY_ENABLED:
      changed = settingsSetHumidityEnabled(cmd.value != 0);
      break;

    case CMD_SET_PRESSURE_ENABLED:
      changed = settingsSetPressureEnabled(cmd.value != 0);
      break;

    case CMD_SET_GAS_ENABLED:
      changed = settingsSetGasEnabled(cmd.value != 0);
      break;

    case CMD_SET_TEMP_UNIT:
      changed = settingsSetTempUnit(cmd.value);
      break;

    case CMD_SET_PRESSURE_UNIT:
      changed = settingsSetPressureUnit(cmd.value);
      break;

    case CMD_SET_REPORT_INTERVAL:
      changed = settingsSetReportInterval(cmd.value);
      if(changed) {
        syncPeriodicityFromSettings();
      }
      break;

    case CMD_SET_PAYLOAD_MODE:
      changed = settingsSetPayloadMode(cmd.value);
      break;

    case CMD_RESET_SETTINGS:
      if(cmd.value == 0xA5) {
        settingsResetDefaults();
        syncPeriodicityFromSettings();
        changed = true;
      }
      break;

    case CMD_NONE:
    default:
      changed = false;
      break;
  }

  if(changed) {
    settingsSave();
    Serial.println(F("[Settings] Updated from downlink."));
    printCurrentSettings();
  } else {
    Serial.println(F("[Settings] Downlink command rejected or no change."));
  }

  return changed;
}

static void printUplinkPayload(const uint8_t* buffer, size_t len, uint8_t payloadMode) {
  if(buffer == nullptr || len == 0) {
    Serial.println(F("[Payload] Empty payload"));
    return;
  }

  Serial.print(F("[Payload] Length: "));
  Serial.println(len);

  if(payloadMode == PAYLOAD_MODE_COMPACT_BIN) {
    Serial.print(F("[Payload] HEX: "));
    for(size_t i = 0; i < len; i++) {
      if(buffer[i] < 0x10) {
        Serial.print('0');
      }
      Serial.print(buffer[i], HEX);
    }
    Serial.println();
  } else {
    Serial.print(F("[Payload] TEXT: "));
    Serial.println((const char*)buffer);
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
    syncPeriodicityFromSettings();
    gLastReportMs = millis() - gPeriodicityMs;
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
  Serial.println(F("Garage Sensor Starting..."));

  settingsBegin();
  syncPeriodicityFromSettings();
  printCurrentSettings();

  if(!sensorBegin()) {
    Serial.println(F("[Sensor] BME68x init failed."));
  } else {
    Serial.println(F("[Sensor] BME68x initialized."));
  }

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
  gLastReportMs = millis() - gPeriodicityMs;
  gNextSchedulerTickMs = 0;
  gSchedulerState = SCHED_WAIT_FOR_JOIN;

  Serial.println(F("[LoRaWAN] Join deferred to scheduler."));
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  uint32_t now = millis();
  if((int32_t)(now - gNextSchedulerTickMs) < 0) {
    return;
  }
  gNextSchedulerTickMs = now + LOOP_SLICE_MS;

  AppSettings* settings = settingsGet();

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

      if((now - gLastReportMs) < gPeriodicityMs) {
        return;
      }

      gSchedulerState = SCHED_TRIGGER_SENSOR;
      break;

    case SCHED_TRIGGER_SENSOR:
      if(!sensorTrigger()) {
        Serial.println(F("[Sensor] Trigger failed."));
        gSchedulerState = SCHED_WAIT_FOR_REPORT;
        return;
      }

      gSensorTriggerMs = now;
      gSchedulerState = SCHED_FETCH_SENSOR;
      break;

    case SCHED_FETCH_SENSOR:
      if((now - gSensorTriggerMs) < SENSOR_FORCED_DELAY_MS) {
        return;
      }

      memset(&gPendingSensorData, 0, sizeof(gPendingSensorData));
      if(!sensorFetch(&gPendingSensorData)) {
        Serial.println(F("[Sensor] Read failed."));
        gSchedulerState = SCHED_WAIT_FOR_REPORT;
        return;
      }

      gSchedulerState = SCHED_BUILD_PAYLOAD;
      break;

    case SCHED_BUILD_PAYLOAD:
      memset(gPendingUplinkBuffer, 0, sizeof(gPendingUplinkBuffer));
      gPendingUplinkLen = payloadEncodeUplink(
        gPendingUplinkBuffer,
        sizeof(gPendingUplinkBuffer),
        &gPendingSensorData,
        settings
      );

      if(gPendingUplinkLen == 0) {
        Serial.println(F("[Payload] Encode failed."));
        gSchedulerState = SCHED_WAIT_FOR_REPORT;
        return;
      }

      printUplinkPayload(gPendingUplinkBuffer, gPendingUplinkLen, settings->payloadMode);
      gSchedulerState = SCHED_SEND_UPLINK;
      break;

    case SCHED_SEND_UPLINK: {
      gPendingDownlinkLen = sizeof(gPendingDownlinkBuffer);
      memset(gPendingDownlinkBuffer, 0, gPendingDownlinkLen);

      Serial.println(F("[LoRaWAN] Sending uplink ..."));

      int16_t state = lwSendReceive(
        gPendingUplinkBuffer,
        gPendingUplinkLen,
        gPendingDownlinkBuffer,
        &gPendingDownlinkLen
      );

      if(state < 0) {
        Serial.print(F("[LoRaWAN] Send failed, code "));
        Serial.println(state);

        if(state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
          gLoRaJoined = false;
          gLastJoinAttemptMs = 0;
          gSchedulerState = SCHED_WAIT_FOR_JOIN;
          Serial.println(F("[LoRaWAN] Session lost. Will retry join."));
          return;
        }

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

        DownlinkCommand cmd = payloadDecodeDownlink(gPendingDownlinkBuffer, gPendingDownlinkLen);
        if(cmd.valid) {
          Serial.print(F("[Downlink] Command type: "));
          Serial.println(cmd.type);
          Serial.print(F("[Downlink] Command value: "));
          Serial.println(cmd.value);

          applyDownlinkCommand(cmd);
        } else {
          Serial.println(F("[Downlink] No valid app command decoded."));
        }
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
