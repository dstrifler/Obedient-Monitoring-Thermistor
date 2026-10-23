#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>

struct SensorData {
  float temperatureC;
  float humidityPct;
  float pressurePa;
  uint32_t gasOhms;
  bool gasValid;  // Gas resistance is usable only with valid/stable heater flags.
};

// Cooperative main-thread monitor: first sample immediately, then every minute.
// Bus transfers are synchronous; the forced-mode measurement wait is scheduled.
void sensorStart();
void sensorPoll();

// Begin configures only; trigger starts one conversion. Fetch returns false
// while pending or on failure, leaving *data unchanged.
bool sensorBegin();
bool sensorTrigger();
bool sensorFetch(SensorData* data);
bool sensorPresent();

#endif
