#include <Arduino.h>

#include "config.h"
#include "eeprom_service.h"
#include "settings_service.h"
#include "sensor_service.h"
#include "codec_service.h"
#include "lorawan_service.h"

// ============================================================
// APP STATE
// ============================================================

static uint32_t gLastReportMs = 0;
static uint32_t gLastJoinAttemptMs = 0;
static bool gLoRaJoined = false;
static uint32_t gPeriodicitySeconds = DEFAULT_UPLINK_INTERVAL_SECONDS;

static const uint32_t JOIN_RETRY_MS = 60000UL;

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
    gLastReportMs = millis() - (gPeriodicitySeconds * 1000UL);
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
  gLastJoinAttemptMs = 0;
  gLastReportMs = millis() - (gPeriodicitySeconds * 1000UL);

  gLoRaJoined = lwActivate();
  gLoRaJoined = gLoRaJoined && lwIsActivated();
  if(gLoRaJoined) {
    Serial.println(F("[LoRaWAN] Initial join successful."));
  } else {
    Serial.println(F("[LoRaWAN] Initial join failed; will retry."));
  }
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  AppSettings* settings = settingsGet();

  syncPeriodicityFromSettings();

  uint32_t now = millis();
  uint32_t intervalMs = gPeriodicitySeconds * 1000UL;

  // ------------------------------------------------------------
  // JOIN MANAGEMENT
  // ------------------------------------------------------------
  gLoRaJoined = lwIsActivated();
  if(!gLoRaJoined) {
    if((now - gLastJoinAttemptMs) >= JOIN_RETRY_MS) {
      gLastJoinAttemptMs = now;
      ensureJoined();
    }

    delay(50);
    return;
  }

  // ------------------------------------------------------------
  // REPORT SCHEDULING
  // ------------------------------------------------------------
  if((now - gLastReportMs) < intervalMs) {
    delay(50);
    return;
  }

  gLastReportMs = now;

  // ------------------------------------------------------------
  // SENSOR READ
  // ------------------------------------------------------------
  SensorData sensorData;
  memset(&sensorData, 0, sizeof(sensorData));

  if(!sensorRead(&sensorData)) {
    Serial.println(F("[Sensor] Read failed."));
    return;
  }

  // ------------------------------------------------------------
  // PAYLOAD BUILD
  // ------------------------------------------------------------
  uint8_t uplinkBuffer[255];
  memset(uplinkBuffer, 0, sizeof(uplinkBuffer));

  size_t uplinkLen = payloadEncodeUplink(
    uplinkBuffer,
    sizeof(uplinkBuffer),
    &sensorData,
    settings
  );

  if(uplinkLen == 0) {
    Serial.println(F("[Payload] Encode failed."));
    return;
  }

  printUplinkPayload(uplinkBuffer, uplinkLen, settings->payloadMode);

  // ------------------------------------------------------------
  // SEND / RECEIVE
  // ------------------------------------------------------------
  uint8_t downlinkBuffer[255];
  memset(downlinkBuffer, 0, sizeof(downlinkBuffer));
  size_t downlinkLen = sizeof(downlinkBuffer);

  Serial.println(F("[LoRaWAN] Sending uplink ..."));

  int16_t state = lwSendReceive(
    uplinkBuffer,
    uplinkLen,
    downlinkBuffer,
    &downlinkLen
  );

  if(state < 0) {
    Serial.print(F("[LoRaWAN] Send failed, code "));
    Serial.println(state);

    if(state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
      gLoRaJoined = false;
      gLastJoinAttemptMs = 0;
      Serial.println(F("[LoRaWAN] Session lost. Will retry join."));
    }

    return;
  }

  Serial.println(F("[LoRaWAN] Uplink sent successfully."));

  // ------------------------------------------------------------
  // DOWNLINK PROCESSING
  // ------------------------------------------------------------
  if(downlinkLen > 0) {
    printDownlinkPayload(downlinkBuffer, downlinkLen);

    DownlinkCommand cmd = payloadDecodeDownlink(downlinkBuffer, downlinkLen);
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
}
