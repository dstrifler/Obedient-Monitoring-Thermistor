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

struct AirQualityData {
  float iaq;
  float co2EquivalentPpm;
  float breathVocEquivalentPpm;
  uint8_t validMask;  // bit 0 IAQ, bit 1 CO2-equivalent, bit 2 VOC-equivalent.
  uint8_t iaqAccuracy;  // Bosch indicator: 0 initializing, 1 low, 2 medium, 3 high.
  bool stabilized;
  bool runIn;
};

struct SensorSnapshot {
  SensorData environment;
  AirQualityData airQuality;
  uint32_t sampleUptimeSeconds;  // Acquisition time since boot; not Unix time.
  bool temperatureHumidityCompensated;
};

// Atomic snapshot of one acquisition, including derived outputs when available.
bool sensorLatestSnapshot(SensorSnapshot* data);

// Main-thread entry points. Default BSEC backend samples in its own worker at
// the Bosch LP rate (3 seconds); raw fallback is cooperative and samples/minute.
void sensorStart();
void sensorPoll();

// Copies the latest validated sample (10 s BSEC / 65 s raw fallback). Missing, failed,
// or stale acquisition returns false and leaves *data unchanged.
bool sensorLatest(SensorData* data);

// Manual raw-backend conversions; unavailable when BSEC owns the sensor.
// Fetch returns false while pending/on failure and leaves *data unchanged.
bool sensorBegin();
bool sensorTrigger();
bool sensorFetch(SensorData* data);
bool sensorPresent();

#endif
