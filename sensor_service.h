#ifndef _SENSOR_SERVICE_H
#define _SENSOR_SERVICE_H

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include <bme68xLibrary.h>

struct SensorData {
  float temperatureC;
  float humidityPct;
  float pressurePa;
  uint32_t gasOhms;
};

static Bme68x gBme;
static bool gSensorPresent = false;

bool sensorBegin() {
#if defined(SENSOR_USE_WIRE1) && (SENSOR_USE_WIRE1 == 1)
  Wire1.begin();
  gBme.begin(SENSOR_I2C_ADDR, Wire1);
#else
  Wire.begin();
  gBme.begin(SENSOR_I2C_ADDR, Wire);
#endif

  gBme.setTPH();
  gBme.setHeaterProf(300, 100);
  gBme.setOpMode(BME68X_FORCED_MODE);
  delay(SENSOR_FORCED_DELAY_MS);

  if(!gBme.fetchData()) {
    gSensorPresent = false;
    return false;
  }

  bme68xData raw;
  if(!gBme.getData(raw)) {
    gSensorPresent = false;
    return false;
  }

  gSensorPresent = true;
  return true;
}

bool sensorRead(SensorData* data) {
  if(data == nullptr) {
    return false;
  }

  if(!gSensorPresent) {
    if(!sensorBegin()) {
      return false;
    }
  }

  gBme.setOpMode(BME68X_FORCED_MODE);
  delay(SENSOR_FORCED_DELAY_MS);

  if(!gBme.fetchData()) {
    gSensorPresent = false;
    return false;
  }

  bme68xData raw;

  if(!gBme.getData(raw)) {
    gSensorPresent = false;
    return false;
  }

  data->temperatureC = raw.temperature;
  data->humidityPct  = raw.humidity;
  data->pressurePa   = raw.pressure;
  data->gasOhms      = raw.gas_resistance;

  return true;
}

bool sensorTrigger() {
  if(!gSensorPresent) {
    if(!sensorBegin()) {
      return false;
    }
  }

  gBme.setOpMode(BME68X_FORCED_MODE);
  return true;
}

bool sensorFetch(SensorData* data) {
  if(data == nullptr) {
    return false;
  }

  if(!gSensorPresent) {
    return false;
  }

  if(!gBme.fetchData()) {
    gSensorPresent = false;
    return false;
  }

  bme68xData raw;
  if(!gBme.getData(raw)) {
    gSensorPresent = false;
    return false;
  }

  data->temperatureC = raw.temperature;
  data->humidityPct  = raw.humidity;
  data->pressurePa   = raw.pressure;
  data->gasOhms      = raw.gas_resistance;

  return true;
}

bool sensorPresent() {
  return gSensorPresent;
}

#endif
