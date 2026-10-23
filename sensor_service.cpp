#include "sensor_service.h"
#include "firmware_features.h"

#if SENSOR_SERVICE_ENABLED
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
static constexpr uint32_t SAMPLE_INTERVAL_MS = 60000;
static constexpr uint32_t DATA_READY_GRACE_MS = 1000;

enum SensorState : uint8_t { SENSOR_STOPPED, SENSOR_START, SENSOR_WAIT, SENSOR_COOLDOWN };
static SensorState gSensorState = SENSOR_STOPPED;
static uint32_t gCycleStartedMs = 0;

static bool sensorFailure(const char* operation) {
  LOG_WARN("Sensor", "%s failed: code=%d; retry in 60 seconds.", operation, int(gBme.status));
  (void)operation;
  gSensorPresent = false;
  gMeasurementPending = false;
  return false;
}

bool sensorBegin() {
  gSensorPresent = false;
  gMeasurementPending = false;
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
    }
    return false;
  }

  if(!isfinite(raw.temperature) || raw.temperature < -40.0f || raw.temperature > 85.0f ||
     !isfinite(raw.humidity) || raw.humidity < 0.0f || raw.humidity > 100.0f ||
     !isfinite(raw.pressure) || raw.pressure < 30000.0f || raw.pressure > 110000.0f) {
    LOG_WARN("Sensor", "Invalid T/P/H reading discarded; retry in 60 seconds.");
    gSensorPresent = false;
    gMeasurementPending = false;
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
  return true;
}

bool sensorPresent() { return gSensorPresent; }

static void logSample(const SensorData& sample) {
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_INFO
  // Integer formatting avoids depending on embedded printf float support.
  const long temperature = lroundf(sample.temperatureC * 100.0f);
  const unsigned long magnitude = temperature < 0 ? -temperature : temperature;
  const unsigned long humidity = lroundf(sample.humidityPct * 100.0f);
  const unsigned long pressure = lroundf(sample.pressurePa);
  if(sample.gasValid) {
    LOG_INFO("Sensor", "T=%s%lu.%02lu C, RH=%lu.%02lu %%, P=%lu.%02lu hPa, gas=%lu ohm",
             temperature < 0 ? "-" : "", magnitude / 100, magnitude % 100,
             humidity / 100, humidity % 100, pressure / 100, pressure % 100,
             (unsigned long)sample.gasOhms);
  } else {
    LOG_INFO("Sensor", "T=%s%lu.%02lu C, RH=%lu.%02lu %%, P=%lu.%02lu hPa, gas=unavailable",
             temperature < 0 ? "-" : "", magnitude / 100, magnitude % 100,
             humidity / 100, humidity % 100, pressure / 100, pressure % 100);
  }
#else
  (void)sample;
#endif
}

void sensorStart() {
  if(gSensorState != SENSOR_STOPPED) { return; }
  gSensorState = SENSOR_START;
  LOG_INFO("Sensor", "Serial sampling enabled: first reading now, then every 60 seconds.");
}

void sensorPoll() {
  if(gSensorState == SENSOR_STOPPED) { return; }
  const uint32_t now = millis();
  if(gSensorState == SENSOR_COOLDOWN) {
    if(now - gCycleStartedMs < SAMPLE_INTERVAL_MS) { return; }
    gSensorState = SENSOR_START;
  }
  if(gSensorState == SENSOR_START) {
    gCycleStartedMs = now;
    if((!gSensorPresent && !sensorBegin()) || !sensorTrigger()) {
      gCycleStartedMs = millis();
      gSensorState = SENSOR_COOLDOWN;
      return;
    }
    gSensorState = SENSOR_WAIT;
    return;
  }
  SensorData sample = {};
  if(sensorFetch(&sample)) {
    logSample(sample);
    gSensorState = SENSOR_COOLDOWN;
  } else if(!gMeasurementPending) {
    gCycleStartedMs = millis();
    gSensorState = SENSOR_COOLDOWN;
  }
}

#else
// Radio-only build: no sensor dependency, bus initialization, or acquisition.
void sensorStart() {}
void sensorPoll() {}
bool sensorBegin() { return false; }
bool sensorTrigger() { return false; }
bool sensorFetch(SensorData*) { return false; }
bool sensorPresent() { return false; }
#endif
