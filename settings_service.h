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

// EEPROM contract note:
// - SETTINGS_VERSION should be incremented when serialized layout or checksum semantics change.
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
// CHECKSUM / SERIALIZATION
// ============================================================

enum SettingsPersistedOffset : size_t {
  SETTINGS_PERSISTED_MAGIC = 0,
  SETTINGS_PERSISTED_VERSION = 4,
  SETTINGS_PERSISTED_TEMP_ENABLED = 5,
  SETTINGS_PERSISTED_HUMIDITY_ENABLED = 6,
  SETTINGS_PERSISTED_PRESSURE_ENABLED = 7,
  SETTINGS_PERSISTED_GAS_ENABLED = 8,
  SETTINGS_PERSISTED_TEMP_UNIT = 9,
  SETTINGS_PERSISTED_PRESSURE_UNIT = 10,
  SETTINGS_PERSISTED_PAYLOAD_MODE = 11,
  SETTINGS_PERSISTED_REPORT_INTERVAL_MIN = 12,
  SETTINGS_PERSISTED_CHECKSUM = 13,
  SETTINGS_PERSISTED_BYTES = 15
};

#if defined(__cplusplus)
static_assert(
  SETTINGS_PERSISTED_BYTES <= EEPROM_SETTINGS_RESERVED_BYTES,
  "Persisted settings schema exceeded reserved EEPROM space; update schema size or EEPROM_SETTINGS_RESERVED_BYTES."
);
#else
#if (SETTINGS_PERSISTED_BYTES > EEPROM_SETTINGS_RESERVED_BYTES)
  #error "Persisted settings schema exceeded reserved EEPROM space; update schema size or EEPROM_SETTINGS_RESERVED_BYTES."
#endif
#endif

static void settingsWriteU16LE(uint8_t* dst, uint16_t value)
{
  dst[0] = (uint8_t)(value & 0xFFu);
  dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t settingsReadU16LE(const uint8_t* src)
{
  return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static void settingsWriteU32LE(uint8_t* dst, uint32_t value)
{
  dst[0] = (uint8_t)(value & 0xFFu);
  dst[1] = (uint8_t)((value >> 8) & 0xFFu);
  dst[2] = (uint8_t)((value >> 16) & 0xFFu);
  dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t settingsReadU32LE(const uint8_t* src)
{
  return (uint32_t)src[0]
    | ((uint32_t)src[1] << 8)
    | ((uint32_t)src[2] << 16)
    | ((uint32_t)src[3] << 24);
}

static void settingsWritePersisted(uint8_t* dst, size_t len, const AppSettings* in)
{
  if((dst == nullptr) || (in == nullptr) || (len < SETTINGS_PERSISTED_BYTES)) return;

  memset(dst, 0, len);

  settingsWriteU32LE(dst + SETTINGS_PERSISTED_MAGIC, in->magic);
  dst[SETTINGS_PERSISTED_VERSION] = in->version;
  dst[SETTINGS_PERSISTED_TEMP_ENABLED] = in->tempEnabled;
  dst[SETTINGS_PERSISTED_HUMIDITY_ENABLED] = in->humidityEnabled;
  dst[SETTINGS_PERSISTED_PRESSURE_ENABLED] = in->pressureEnabled;
  dst[SETTINGS_PERSISTED_GAS_ENABLED] = in->gasEnabled;
  dst[SETTINGS_PERSISTED_TEMP_UNIT] = in->tempUnit;
  dst[SETTINGS_PERSISTED_PRESSURE_UNIT] = in->pressureUnit;
  dst[SETTINGS_PERSISTED_PAYLOAD_MODE] = in->payloadMode;
  dst[SETTINGS_PERSISTED_REPORT_INTERVAL_MIN] = in->reportIntervalMin;
  settingsWriteU16LE(dst + SETTINGS_PERSISTED_CHECKSUM, in->checksum);
}

static void settingsReadPersisted(const uint8_t* src, size_t len, AppSettings* out)
{
  if((src == nullptr) || (out == nullptr) || (len < SETTINGS_PERSISTED_BYTES)) return;

  out->magic = settingsReadU32LE(src + SETTINGS_PERSISTED_MAGIC);
  out->version = src[SETTINGS_PERSISTED_VERSION];
  out->tempEnabled = src[SETTINGS_PERSISTED_TEMP_ENABLED];
  out->humidityEnabled = src[SETTINGS_PERSISTED_HUMIDITY_ENABLED];
  out->pressureEnabled = src[SETTINGS_PERSISTED_PRESSURE_ENABLED];
  out->gasEnabled = src[SETTINGS_PERSISTED_GAS_ENABLED];
  out->tempUnit = src[SETTINGS_PERSISTED_TEMP_UNIT];
  out->pressureUnit = src[SETTINGS_PERSISTED_PRESSURE_UNIT];
  out->payloadMode = src[SETTINGS_PERSISTED_PAYLOAD_MODE];
  out->reportIntervalMin = src[SETTINGS_PERSISTED_REPORT_INTERVAL_MIN];
  out->checksum = settingsReadU16LE(src + SETTINGS_PERSISTED_CHECKSUM);
}

static uint16_t settingsChecksum(const AppSettings* s)
{
  uint16_t sum = 0;

  sum += (uint16_t)(s->magic & 0xFF);
  sum += (uint16_t)((s->magic >> 8) & 0xFF);
  sum += (uint16_t)((s->magic >> 16) & 0xFF);
  sum += (uint16_t)((s->magic >> 24) & 0xFF);

  sum += s->version;

  sum += s->tempEnabled;
  sum += s->humidityEnabled;
  sum += s->pressureEnabled;
  sum += s->gasEnabled;

  sum += s->tempUnit;
  sum += s->pressureUnit;
  sum += s->payloadMode;
  sum += s->reportIntervalMin;

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
  uint8_t persisted[SETTINGS_PERSISTED_BYTES];

  gSettings.checksum = settingsChecksum(&gSettings);
  settingsWritePersisted(persisted, sizeof(persisted), &gSettings);

  for(size_t i = 0; i < sizeof(persisted); i++) {
    EEPROM.write(EEPROM_ADDR_SETTINGS_BASE + i, persisted[i]);
  }
}

void settingsLoad()
{
  uint8_t persisted[SETTINGS_PERSISTED_BYTES];

  for(size_t i = 0; i < sizeof(persisted); i++) {
    persisted[i] = EEPROM.read(EEPROM_ADDR_SETTINGS_BASE + i);
  }

  // Explicit version migration gate:
  // if serialized schema changes, bump SETTINGS_VERSION and firmware will
  // reset/migrate from here rather than interpreting an older layout.
  if(persisted[SETTINGS_PERSISTED_VERSION] != SETTINGS_VERSION) {
    settingsResetDefaults();
    settingsSave();
    return;
  }

  settingsReadPersisted(persisted, sizeof(persisted), &gSettings);

  if(!settingsValidate(&gSettings)) {
    settingsResetDefaults();
    settingsSave();
  }
}

void settingsBegin()
{
  // EEPROM must already be initialized by app bootstrap (eepromBootstrap()).
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
