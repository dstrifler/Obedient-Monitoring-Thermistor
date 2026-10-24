#include "sensor_service.h"
#include "firmware_features.h"
#include "storage_service.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "log_service.h"

// Backends own acquisition and publish complete records directly to storage.
// The facade services persistence and formatted Serial on the main thread.
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
#include "bsec_service.h"
static void sensorBackendStart() { bsecStart(); }
static void sensorBackendPoll() { bsecPoll(); }
bool sensorLatestSnapshot(SensorSnapshot* data) { return bsecLatest(data); }
bool sensorLatest(SensorData* data) {
  SensorSnapshot snapshot = {};
  if(data == nullptr || !bsecLatest(&snapshot)) { return false; }
  *data = snapshot.environment;
  return true;
}
bool sensorPresent() { return bsecPresent(); }
// The BSEC worker exclusively owns the bus; manual conversions are unavailable.
bool sensorBegin() { return false; }
bool sensorTrigger() { return false; }
bool sensorFetch(SensorData*) { return false; }
#elif SENSOR_SERVICE_ENABLED
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <bme68xLibrary.h>
#include "config.h"
#include "log_service.h"

static_assert(SENSOR_I2C_ADDR == 0x76 || SENSOR_I2C_ADDR == 0x77,
              "BME68x I2C address must be 0x76 or 0x77");

static Bme68x gBme;
static bool gWireStarted = false;
static bool gSensorPresent = false;
static bool gMeasurementPending = false;
static uint32_t gMeasurementStartedMs = 0;
static uint32_t gMeasurementWaitMs = 0;
static constexpr uint16_t HEATER_TEMPERATURE_C = 300;
static constexpr uint16_t HEATER_DURATION_MS = 100;
static constexpr uint32_t SAMPLE_INTERVAL_MS = 3000;
static constexpr uint32_t DATA_READY_GRACE_MS = 1000;
static constexpr uint32_t SAMPLE_MAX_AGE_MS = 10000;
static SensorData gLatestSample = {};
static bool gHaveLatestSample = false;
static uint32_t gLatestSampleMs = 0;
static uint32_t gLatestSampleUptimeSeconds = 0;
static uint32_t gUptimeTick = 0;
static uint64_t gUptimeMs = 0;

static uint64_t sensorUptimeMs() {
  const uint32_t now = millis();
  gUptimeMs += uint32_t(now - gUptimeTick);
  gUptimeTick = now;
  return gUptimeMs;
}

enum SensorState : uint8_t { SENSOR_STOPPED, SENSOR_START, SENSOR_WAIT, SENSOR_COOLDOWN };
static SensorState gSensorState = SENSOR_STOPPED;
static uint32_t gCycleStartedMs = 0;
static uint32_t gCycleWaitMs = SAMPLE_INTERVAL_MS;

static bool sensorFailure(const char* operation) {
  LOG_WARN("Sensor", "%s failed: code=%d; retry in 60 seconds.", operation, int(gBme.status));
  (void)operation;
  gSensorPresent = false;
  gMeasurementPending = false;
  gHaveLatestSample = false;
  return false;
}

bool sensorBegin() {
  sensorUptimeMs();
  gSensorPresent = false;
  gMeasurementPending = false;
  gHaveLatestSample = false;
#if defined(SENSOR_USE_WIRE1) && (SENSOR_USE_WIRE1 == 1)
  if(!gWireStarted) { Wire1.begin(); gWireStarted = true; }
  gBme.begin(SENSOR_I2C_ADDR, Wire1);
#else
  if(!gWireStarted) { Wire.begin(); gWireStarted = true; }
  gBme.begin(SENSOR_I2C_ADDR, Wire);
#endif
  if(gBme.status != BME68X_OK) { return sensorFailure("Initialization"); }
  gBme.setTPH();
  if(gBme.status != BME68X_OK) { return sensorFailure("TPH configuration"); }
  gBme.setHeaterProf(HEATER_TEMPERATURE_C, HEATER_DURATION_MS);
  if(gBme.status != BME68X_OK) { return sensorFailure("Heater configuration"); }
  gSensorPresent = true;
  LOG_INFO("Sensor", "BME68x ready at I2C address 0x%02X.", unsigned(SENSOR_I2C_ADDR));
  return true;
}

bool sensorTrigger() {
  if(!gSensorPresent || gMeasurementPending) { return false; }
  const uint32_t tphUs = gBme.getMeasDur(BME68X_FORCED_MODE);
  if(tphUs == 0 || tphUs > 1000000UL) {
    LOG_WARN("Sensor", "Invalid measurement duration; retry in 60 seconds.");
    gSensorPresent = false;
    gHaveLatestSample = false;
    return false;
  }
  // Bosch's duration covers T/P/H only; add the programmed gas-heater time.
  gMeasurementWaitMs = (tphUs + 999UL) / 1000UL + HEATER_DURATION_MS + 5UL;
  gBme.setOpMode(BME68X_FORCED_MODE);
  if(gBme.status != BME68X_OK) { return sensorFailure("Measurement trigger"); }
  gMeasurementStartedMs = millis();
  gMeasurementPending = true;
  return true;
}

bool sensorFetch(SensorData* data) {
  if(data == nullptr || !gSensorPresent || !gMeasurementPending) { return false; }
  const uint32_t elapsed = millis() - gMeasurementStartedMs;
  if(elapsed < gMeasurementWaitMs) { return false; }

  const uint8_t fields = gBme.fetchData();
  if(gBme.status < BME68X_OK) { return sensorFailure("Data fetch"); }
  bme68xData raw = {};
  if(fields > 0) {
    // getData returns the number of fields REMAINING. In forced mode a valid
    // single sample returns zero, so it must not be tested as a success flag.
    gBme.getData(raw);
  }
  if(fields == 0 || !(raw.status & BME68X_NEW_DATA_MSK)) {
    if(elapsed >= gMeasurementWaitMs + DATA_READY_GRACE_MS) {
      LOG_WARN("Sensor", "Measurement timed out without fresh data; retry in 60 seconds.");
      gSensorPresent = false;
      gMeasurementPending = false;
      gHaveLatestSample = false;
    }
    return false;
  }

  if(!isfinite(raw.temperature) || raw.temperature < -40.0f || raw.temperature > 85.0f ||
     !isfinite(raw.humidity) || raw.humidity < 0.0f || raw.humidity > 100.0f ||
     !isfinite(raw.pressure) || raw.pressure < 30000.0f || raw.pressure > 110000.0f) {
    LOG_WARN("Sensor", "Invalid T/P/H reading discarded; retry in 60 seconds.");
    gSensorPresent = false;
    gMeasurementPending = false;
    gHaveLatestSample = false;
    return false;
  }

  SensorData sample = {};
  sample.temperatureC = raw.temperature;
  sample.humidityPct = raw.humidity;
  sample.pressurePa = raw.pressure;
  const uint8_t gasMask = BME68X_GASM_VALID_MSK | BME68X_HEAT_STAB_MSK;
  sample.gasValid = (raw.status & gasMask) == gasMask &&
                    isfinite(raw.gas_resistance) && raw.gas_resistance > 0 &&
                    double(raw.gas_resistance) <= double(UINT32_MAX);
  if(sample.gasValid) { sample.gasOhms = uint32_t(raw.gas_resistance); }
  *data = sample;
  gMeasurementPending = false;
  gLatestSample = sample;
  // Forced-mode data can remain in the chip across a long blocking radio
  // call. Age from estimated conversion completion, not delayed collection.
  gLatestSampleMs = gMeasurementStartedMs + gMeasurementWaitMs;
  const uint64_t nowUptime = sensorUptimeMs();
  gLatestSampleUptimeSeconds = uint32_t((nowUptime - uint32_t(gUptimeTick - gMeasurementStartedMs) + gMeasurementWaitMs) / 1000ULL);
  gHaveLatestSample = true;
  return true;
}

bool sensorPresent() { return gSensorPresent; }

bool sensorLatest(SensorData* data) {
  if(data == nullptr || !gHaveLatestSample || !gSensorPresent ||
     uint32_t(millis() - gLatestSampleMs) > SAMPLE_MAX_AGE_MS) { return false; }
  *data = gLatestSample;
  return true;
}

bool sensorLatestSnapshot(SensorSnapshot* data) {
  SensorSnapshot snapshot = {};
  if(data == nullptr || !sensorLatest(&snapshot.environment)) { return false; }
  snapshot.sampleUptimeSeconds = gLatestSampleUptimeSeconds;
  *data = snapshot;
  return true;
}

static void sensorBackendStart() {
  if(gSensorState != SENSOR_STOPPED) { return; }
  gSensorState = SENSOR_START;
  LOG_INFO("Sensor", "Raw acquisition enabled: first reading now, then every three seconds.");
}

static void sensorBackendPoll() {
  if(gSensorState == SENSOR_STOPPED) { return; }
  sensorUptimeMs();
  const uint32_t now = millis();
  if(gSensorState == SENSOR_COOLDOWN) {
    if(now - gCycleStartedMs < gCycleWaitMs) { return; }
    gSensorState = SENSOR_START;
  }
  if(gSensorState == SENSOR_START) {
    gCycleStartedMs = now;
    if((!gSensorPresent && !sensorBegin()) || !sensorTrigger()) {
      gCycleStartedMs = millis();
      gCycleWaitMs = 60000;
      gSensorState = SENSOR_COOLDOWN;
      return;
    }
    gSensorState = SENSOR_WAIT;
    return;
  }
  SensorData sample = {};
  if(sensorFetch(&sample)) {
    SensorSnapshot record = {};
    record.environment = sample;
    record.sampleUptimeSeconds = gLatestSampleUptimeSeconds;
    storageAppend(record, gLatestSampleMs);
    gCycleWaitMs = SAMPLE_INTERVAL_MS;
    gSensorState = SENSOR_COOLDOWN;
  } else if(!gMeasurementPending) {
    gCycleWaitMs = 60000;
    gCycleStartedMs = millis();
    gSensorState = SENSOR_COOLDOWN;
  }
}

#else
// Radio-only build: no sensor dependency, bus initialization, or acquisition.
static void sensorBackendStart() {}
static void sensorBackendPoll() {}
bool sensorBegin() { return false; }
bool sensorTrigger() { return false; }
bool sensorFetch(SensorData*) { return false; }
bool sensorLatest(SensorData*) { return false; }
bool sensorLatestSnapshot(SensorSnapshot*) { return false; }
bool sensorPresent() { return false; }
#endif

namespace {
bool facadeStarted = false;
uint32_t lastSerialMs = 0;
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
void formatReading(char* out, size_t size, double value, unsigned decimals,
                   const char* unit, bool available) {
  if(!available) { snprintf(out, size, "unavailable"); return; }
  const uint32_t factors[] = {1, 10, 100, 1000};
  const uint32_t factor = factors[decimals];
  const double magnitude = fabs(value) * factor;
  if(!isfinite(magnitude) || magnitude > UINT32_MAX) { snprintf(out, size, "out of display range"); return; }
  const uint32_t scaled = uint32_t(magnitude + 0.5);
  if(decimals) {
    snprintf(out, size, "%s%lu.%0*lu %s", value < 0 ? "-" : "",
      (unsigned long)(scaled / factor), int(decimals), (unsigned long)(scaled % factor), unit);
  } else {
    snprintf(out, size, "%s%lu %s", value < 0 ? "-" : "", (unsigned long)scaled, unit);
  }
}
#endif
void printReading(const SensorSnapshot& sample, const AppSettings& settings) {
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  const auto& e = sample.environment;
  const auto& a = sample.airQuality;
  const uint8_t mask = sensorValidMask(sample);
  const float temperature = settings.temperature == TemperatureUnit::Fahrenheit ? e.temperatureC * 1.8f + 32 : e.temperatureC;
  float pressure = e.pressurePa;
  const char* pressureUnit = "Pa";
  switch(settings.pressure) {
    case PressureUnit::Hpa: pressure /= 100; pressureUnit = "mbar"; break;
    case PressureUnit::InHg: pressure /= 3386.389f; pressureUnit = "inHg"; break;
    case PressureUnit::Kpa: pressure /= 1000; pressureUnit = "kPa"; break;
    case PressureUnit::MmHg: pressure /= 133.3224f; pressureUnit = "mmHg"; break;
    case PressureUnit::Pa: break;
  }
  char temp[32], humidity[32], pres[32], gas[32], iaq[32], co2[32], voc[40];
  formatReading(temp, sizeof(temp), temperature, 2,
    settings.temperature == TemperatureUnit::Fahrenheit ? "F" : "C", mask & 1);
  formatReading(humidity, sizeof(humidity), e.humidityPct, 2, "%RH", mask & 2);
  formatReading(pres, sizeof(pres), pressure, 2, pressureUnit, mask & 4);
  formatReading(gas, sizeof(gas), settings.gas == GasUnit::KiloOhms ? double(e.gasOhms) / 1000 : e.gasOhms,
    settings.gas == GasUnit::KiloOhms ? 3 : 0, settings.gas == GasUnit::KiloOhms ? "kohm" : "ohm", mask & 8);
  formatReading(iaq, sizeof(iaq), a.iaq, 1, "", mask & 16);
  formatReading(co2, sizeof(co2), a.co2EquivalentPpm, 0, "ppm", mask & 32);
  formatReading(voc, sizeof(voc), double(a.breathVocEquivalentPpm) * (settings.voc == VocUnit::Ppb ? 1000 : 1),
    settings.voc == VocUnit::Ppb ? 1 : 3, settings.voc == VocUnit::Ppb ? "ppb" : "ppm", mask & 64);
  LOG_INFO("Sensor", "T=%s, RH=%s, P=%s, valid=0x%02X.", temp, humidity, pres, unsigned(mask));
  LOG_INFO("Sensor", "Gas=%s, IAQ=%s(accuracy=%u), CO2eq=%s, VOCeq=%s.",
    gas, iaq, unsigned(a.iaqAccuracy), co2, voc);
#else
  (void)sample; (void)settings;
#endif
}
}
void sensorStart() {
  if(facadeStarted) { return; }
  facadeStarted = true;
  lastSerialMs = millis() - 60000UL;
  sensorBackendStart();
}
void sensorPoll(const AppSettings& settings) {
  if(!facadeStarted) { return; }
  sensorBackendPoll();
  if(uint32_t(millis() - lastSerialMs) >= 60000UL) {
    SensorSnapshot sample = {};
    if(sensorLatestSnapshot(&sample)) { lastSerialMs = millis(); printReading(sample, settings); }
  }
}
bool sensorStop() {
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
  return bsecStop();
#elif SENSOR_SERVICE_ENABLED
  gSensorState = SENSOR_STOPPED;
  gMeasurementPending = false;
  return true;
#else
  return true;
#endif
}
