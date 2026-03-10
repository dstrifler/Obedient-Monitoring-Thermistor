#ifndef _SETTINGS_SERVICE_H
#define _SETTINGS_SERVICE_H

#include <Arduino.h>
#include <EEPROM.h>
#include "config.h"
#include "eeprom_service.h"

// ============================================================
// ENUMS
// ============================================================

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

// EEPROM contract note:
// - SETTINGS_VERSION should be incremented when AppSettings layout changes.
// - Keep AppSettings within EEPROM_SETTINGS_RESERVED_BYTES, or increase the
//   reservation in eeprom_service.h before shipping the new firmware.
#if defined(__cplusplus)
static_assert(
  sizeof(AppSettings) <= EEPROM_SETTINGS_RESERVED_BYTES,
  "AppSettings exceeded reserved EEPROM space; update AppSettings size or EEPROM_SETTINGS_RESERVED_BYTES."
);
#else
#if (sizeof(AppSettings) > EEPROM_SETTINGS_RESERVED_BYTES)
  #error "AppSettings exceeded reserved EEPROM space; update AppSettings size or EEPROM_SETTINGS_RESERVED_BYTES."
#endif
#endif

static AppSettings gSettings;

// ============================================================
// CHECKSUM
// ============================================================

static uint16_t settingsChecksum(const AppSettings* s)
{
  const uint8_t* p = (const uint8_t*)s;
  uint16_t sum = 0;

  for(size_t i = 0; i < (sizeof(AppSettings) - sizeof(s->checksum)); i++) {
    sum += p[i];
  }

  return sum;
}

// ============================================================
// VALIDATION
// ============================================================

static bool settingsIsValidInterval(uint8_t minutes)
{
  switch(minutes) {
    case 5:
    case 10:
    case 15:
    case 20:
    case 25:
    case 30:
    case 35:
    case 40:
    case 45:
    case 50:
    case 55:
    case 60:
      return true;
    default:
      return false;
  }
}

bool settingsIsValidReportInterval(uint8_t minutes)
{
  return settingsIsValidInterval(minutes);
}

bool settingsIsValidTempUnit(uint8_t unit)
{
  return (unit <= TEMP_UNIT_F);
}

bool settingsIsValidPressureUnit(uint8_t unit)
{
  return (unit <= PRESSURE_UNIT_PA);
}

bool settingsIsValidPayloadMode(uint8_t mode)
{
  return (mode <= PAYLOAD_MODE_VERBOSE_DEBUG);
}

static bool settingsValidate(const AppSettings* s)
{
  if(s->magic != SETTINGS_MAGIC) return false;
  if(s->version != SETTINGS_VERSION) return false;

  if(s->tempEnabled > 1) return false;
  if(s->humidityEnabled > 1) return false;
  if(s->pressureEnabled > 1) return false;
  if(s->gasEnabled > 1) return false;

  if(!settingsIsValidTempUnit(s->tempUnit)) return false;
  if(!settingsIsValidPressureUnit(s->pressureUnit)) return false;
  if(!settingsIsValidPayloadMode(s->payloadMode)) return false;

  if(!settingsIsValidReportInterval(s->reportIntervalMin)) return false;

  if(s->checksum != settingsChecksum(s)) return false;

  return true;
}

// ============================================================
// DEFAULT SETTINGS
// ============================================================

void settingsResetDefaults()
{
  memset(&gSettings, 0, sizeof(gSettings));

  gSettings.magic = SETTINGS_MAGIC;
  gSettings.version = SETTINGS_VERSION;

  gSettings.tempEnabled = 1;
  gSettings.humidityEnabled = 1;
  gSettings.pressureEnabled = 1;
  gSettings.gasEnabled = 1;

  gSettings.tempUnit = TEMP_UNIT_C;
  gSettings.pressureUnit = PRESSURE_UNIT_PA;
  gSettings.payloadMode = PAYLOAD_MODE_COMPACT_BIN;

  gSettings.reportIntervalMin = 15;

  gSettings.checksum = settingsChecksum(&gSettings);
}

// ============================================================
// EEPROM SAVE / LOAD
// ============================================================

void settingsSave()
{
  gSettings.checksum = settingsChecksum(&gSettings);
  EEPROM.put(EEPROM_ADDR_SETTINGS_BASE, gSettings);
}

void settingsLoad()
{
  EEPROM.get(EEPROM_ADDR_SETTINGS_BASE, gSettings);

  if(!settingsValidate(&gSettings)) {
    settingsResetDefaults();
    settingsSave();
  }
}

void settingsBegin()
{
  EEPROM.init();
  EEPROM.setLength(EEPROM_TOTAL_BYTES);

  settingsLoad();
}

// ============================================================
// ACCESS
// ============================================================

AppSettings* settingsGet()
{
  return &gSettings;
}

// ============================================================
// SETTERS
// ============================================================

bool settingsSetTempEnabled(bool enabled)
{
  uint8_t newValue = enabled ? 1 : 0;
  bool changed = (gSettings.tempEnabled != newValue);
  gSettings.tempEnabled = newValue;
  return changed;
}

bool settingsSetHumidityEnabled(bool enabled)
{
  uint8_t newValue = enabled ? 1 : 0;
  bool changed = (gSettings.humidityEnabled != newValue);
  gSettings.humidityEnabled = newValue;
  return changed;
}

bool settingsSetPressureEnabled(bool enabled)
{
  uint8_t newValue = enabled ? 1 : 0;
  bool changed = (gSettings.pressureEnabled != newValue);
  gSettings.pressureEnabled = newValue;
  return changed;
}

bool settingsSetGasEnabled(bool enabled)
{
  uint8_t newValue = enabled ? 1 : 0;
  bool changed = (gSettings.gasEnabled != newValue);
  gSettings.gasEnabled = newValue;
  return changed;
}

bool settingsSetTempUnit(uint8_t unit)
{
  if(!settingsIsValidTempUnit(unit)) return false;
  bool changed = (gSettings.tempUnit != unit);
  gSettings.tempUnit = unit;
  return changed;
}

bool settingsSetPressureUnit(uint8_t unit)
{
  if(!settingsIsValidPressureUnit(unit)) return false;
  bool changed = (gSettings.pressureUnit != unit);
  gSettings.pressureUnit = unit;
  return changed;
}

bool settingsSetPayloadMode(uint8_t mode)
{
  if(!settingsIsValidPayloadMode(mode)) return false;
  bool changed = (gSettings.payloadMode != mode);
  gSettings.payloadMode = mode;
  return changed;
}

bool settingsSetReportInterval(uint8_t minutes)
{
  if(!settingsIsValidReportInterval(minutes)) return false;
  bool changed = (gSettings.reportIntervalMin != minutes);
  gSettings.reportIntervalMin = minutes;
  return changed;
}

#endif
