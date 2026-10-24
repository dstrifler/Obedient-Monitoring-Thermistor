#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>

#include "sensor_types.h"
#include "settings_defaults.h"

// Atomic snapshot of one acquisition, including derived outputs when available.
// A successful acquisition can be partial; check sensorValidMask before use.
bool sensorLatestSnapshot(SensorSnapshot* data);

// Main-thread lifecycle/persistence/Serial. Completed acquisitions are committed
// directly to storage by the backend; reporting never controls acquisition.
void sensorStart();
void sensorPoll(const AppSettings& settings = settingsDefaults());
// Cooperative stop: caller polls until the acquisition worker has exited.
bool sensorStop();

// Copies the latest acquisition (10 seconds), with independent field validity.
// Invalid fields follow sensorValidMask rules; they do not fail the whole copy.
// Missing, failed, or stale acquisition returns false and leaves *data unchanged.
bool sensorLatest(SensorData* data);

// Manual raw-backend conversions; unavailable when BSEC owns the sensor.
// Fetch returns false while pending/on failure and leaves *data unchanged.
// A completed conversion returns true even when some numeric fields are invalid.
bool sensorBegin();
bool sensorTrigger();
bool sensorFetch(SensorData* data);
bool sensorPresent();

#endif
