#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>

struct SensorData {
  float temperatureC;
  float humidityPct;
  float pressurePa;
  uint32_t gasOhms;
};

bool sensorBegin();
bool sensorTrigger();
bool sensorFetch(SensorData* data);
bool sensorPresent();

#endif
