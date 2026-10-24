#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>

struct SensorData {
  float temperatureC;
  float humidityPct;
  float pressurePa;
  uint32_t gasOhms;
  bool gasValid;
};
struct AirQualityData {
  float iaq;
  float co2EquivalentPpm;
  float breathVocEquivalentPpm;
  uint8_t validMask;
  uint8_t iaqAccuracy;
  bool stabilized;
  bool runIn;
};
struct SensorSnapshot {
  SensorData environment;
  AirQualityData airQuality;
  uint32_t sampleUptimeSeconds;
  bool temperatureHumidityCompensated;
};

// Bits 0..6: temperature, humidity, pressure, gas, IAQ, CO2eq, VOCeq.
uint8_t sensorValidMask(const SensorSnapshot& sample);
uint8_t sensorQualityByte(const SensorSnapshot& sample);

#endif
