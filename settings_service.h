#ifndef SETTINGS_SERVICE_H
#define SETTINGS_SERVICE_H

#include <stdint.h>

enum TempUnit : uint8_t {
  TEMP_UNIT_C = 0,
  TEMP_UNIT_F = 1
};

enum PressureUnit : uint8_t {
  PRESSURE_UNIT_HPA = 0,
  PRESSURE_UNIT_INHG = 1,
  PRESSURE_UNIT_PA = 2
};

enum PayloadMode : uint8_t {
  PAYLOAD_MODE_COMPACT_BIN = 0,
  PAYLOAD_MODE_COMPACT_JSON = 1,
  PAYLOAD_MODE_VERBOSE_DEBUG = 2
};
// Settings contract note:
// - COMPACT_BIN is the preferred transport mode for low-data-rate LoRaWAN links.
// - COMPACT_JSON / VERBOSE_DEBUG are best-effort diagnostics and may be dropped
//   by payload-size preflight checks when regional data-rate limits are tight.

// ============================================================
// SETTINGS STRUCT
// ============================================================

struct AppSettings {
  uint32_t magic;
  uint8_t version;

  uint8_t tempEnabled;
  uint8_t humidityEnabled;
  uint8_t pressureEnabled;
  uint8_t gasEnabled;

  uint8_t tempUnit;
  uint8_t pressureUnit;
  uint8_t payloadMode;
  uint8_t reportIntervalMin;

  uint16_t checksum;
};

bool settingsIsValidReportInterval(uint8_t minutes);
bool settingsIsValidTempUnit(uint8_t unit);
bool settingsIsValidPressureUnit(uint8_t unit);
bool settingsIsValidPayloadMode(uint8_t mode);
void settingsResetDefaults();
void settingsSave();
void settingsLoad();
void settingsBegin();
AppSettings* settingsGet();
bool settingsSetTempEnabled(bool enabled);
bool settingsSetHumidityEnabled(bool enabled);
bool settingsSetPressureEnabled(bool enabled);
bool settingsSetGasEnabled(bool enabled);
bool settingsSetTempUnit(uint8_t unit);
bool settingsSetPressureUnit(uint8_t unit);
bool settingsSetPayloadMode(uint8_t mode);
bool settingsSetReportInterval(uint8_t minutes);

#endif
