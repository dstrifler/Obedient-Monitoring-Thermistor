#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include "sensor_types.h"
#include "app_settings.h"
#include <stddef.h>
#include <stdint.h>

static constexpr size_t STORAGE_CAPACITY = 1200;
static constexpr uint32_t STORAGE_HISTORY_MS = 3600000UL;
static constexpr uint32_t STORAGE_FRESH_MS = 10000UL;
struct StoredReading { SensorSnapshot sample; uint32_t acquiredMs; };

// Persistent operations are main-thread only. RAM record access is mutex protected.
bool storageBegin();
void storageClearHistory();
void storageSetCommitNotification(void (*notify)());
bool storageAppend(const SensorSnapshot& sample, uint32_t acquiredMs);
size_t storageCount();
uint32_t storageGeneration();
bool storageFirstReadingMs(uint32_t* time);
bool storageRead(size_t newestOffset, StoredReading* reading);
bool storageLatestSnapshot(SensorSnapshot* sample, uint32_t* ageMs = nullptr);
bool storageLoadSettings(AppSettings* settings);
bool storageSaveSettings(const AppSettings& settings);
uint16_t storageLoadCalibration(uint8_t* data, size_t capacity, uint32_t profile);
bool storageSaveCalibration(const uint8_t* data, uint16_t size, uint32_t profile);
bool storageNonceBegin();
bool storageNonceLoad(uint8_t* data, size_t capacity);
bool storageNonceSave(const uint8_t* data);
// Caller must stop sensor work and radio work first. No reset marker is written.
bool storageFactoryErase();

#endif
