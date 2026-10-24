#include "storage_service.h"
#include "eeprom_service.h"
#include "bsec_state.h"
#include "nonce_store.h"
#include "firmware_features.h"
#include "settings_defaults.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <mbed.h>
#include <string.h>

namespace {
rtos::Mutex historyMutex;
StoredReading history[STORAGE_CAPACITY];
static_assert(sizeof(history) <= 57600, "Review the one-hour ring RAM budget");
size_t nextRecord = 0, recordCount = 0;
uint32_t generation = 0, firstReading = 0;
bool haveFirst = false;
void (*notification)() = nullptr;
constexpr size_t SETTINGS_BYTES = 32;
constexpr uint32_t SETTINGS_TAG = 0x33544553UL;

uint32_t crc(const uint8_t* data, size_t size) {
  uint32_t value = UINT32_MAX;
  for(size_t i = 0; i < size; ++i) {
    value ^= data[i];
    for(unsigned bit = 0; bit < 8; ++bit) { value = (value >> 1) ^ (0xEDB88320UL & (0UL - (value & 1))); }
  }
  return ~value;
}
void write32(uint8_t* data, uint32_t value) {
  for(unsigned i = 0; i < 4; ++i) { data[i] = uint8_t(value >> (8 * i)); }
}
uint32_t read32(const uint8_t* data) {
  return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
void prune() {
  while(recordCount) {
    const size_t oldest = (nextRecord + STORAGE_CAPACITY - recordCount) % STORAGE_CAPACITY;
    if(uint32_t(millis() - history[oldest].acquiredMs) < STORAGE_HISTORY_MS) { break; }
    --recordCount;
  }
}
}

bool storageBegin() { storageClearHistory(); return eepromBootstrap(); }
void storageClearHistory() {
  historyMutex.lock();
  nextRecord = recordCount = 0;
  generation = firstReading = 0;
  haveFirst = false;
  historyMutex.unlock();
}
void storageSetCommitNotification(void (*notify)()) { notification = notify; }
bool storageAppend(const SensorSnapshot& sample, uint32_t acquiredMs) {
  if(!sensorValidMask(sample) || uint32_t(millis() - acquiredMs) > STORAGE_FRESH_MS) { return false; }
  historyMutex.lock();
  prune();
  if(recordCount && history[(nextRecord + STORAGE_CAPACITY - 1) % STORAGE_CAPACITY].acquiredMs == acquiredMs) {
    historyMutex.unlock(); return false;
  }
  history[nextRecord] = {sample, acquiredMs};
  nextRecord = (nextRecord + 1) % STORAGE_CAPACITY;
  if(recordCount < STORAGE_CAPACITY) { ++recordCount; }
  if(!haveFirst) { firstReading = acquiredMs; haveFirst = true; }
  ++generation;
  historyMutex.unlock();
  // Notification runs after the whole record commits. It only wakes policy;
  // acquisition never formats packets, logs, writes flash, or waits on RF.
  if(notification) { notification(); }
  return true;
}
size_t storageCount() {
  historyMutex.lock(); prune(); const size_t count = recordCount; historyMutex.unlock(); return count;
}
uint32_t storageGeneration() {
  historyMutex.lock(); const uint32_t value = generation; historyMutex.unlock(); return value;
}
bool storageFirstReadingMs(uint32_t* time) {
  if(!time) { return false; }
  historyMutex.lock(); const bool valid = haveFirst; if(valid) { *time = firstReading; } historyMutex.unlock(); return valid;
}
bool storageRead(size_t offset, StoredReading* reading) {
  if(!reading) { return false; }
  historyMutex.lock(); prune();
  const bool valid = offset < recordCount;
  if(valid) { *reading = history[(nextRecord + STORAGE_CAPACITY - 1 - offset) % STORAGE_CAPACITY]; }
  historyMutex.unlock(); return valid;
}
bool storageLatestSnapshot(SensorSnapshot* sample, uint32_t* ageMs) {
  StoredReading record = {};
  if(!sample || !storageRead(0, &record)) { return false; }
  const uint32_t age = millis() - record.acquiredMs;
  if(ageMs) { *ageMs = age; }
  if(age > STORAGE_FRESH_MS) { return false; }
  *sample = record.sample; return true;
}
bool storageLoadSettings(AppSettings* settings) {
  if(!settings || !eepromBootstrap()) { return false; }
  uint8_t data[SETTINGS_BYTES];
  for(size_t i = 0; i < sizeof(data); ++i) { data[i] = EEPROM.read(EEPROM_ADDR_SETTINGS_BASE + i); }
  if(read32(data) != SETTINGS_TAG || data[4] != 3 || data[5] != 7 || read32(data + 28) != crc(data, 28)) {
    // The old v2 record had no reporting-mode/log/gas/VOC preferences. Preserve
    // its valid unit choices and fixed-interval preset, filling new fields from
    // defaults. Migration is read-only until an explicit settings save.
    uint16_t sum = 0;
    for(unsigned i = 0; i < 13; ++i) { sum += data[i]; }
    if(read32(data) != 0x47534150UL || data[4] != 2 ||
       data[5] > 1 || data[6] > 1 || data[7] > 1 || data[8] > 1 ||
       data[9] > 1 || data[10] > 2 || data[11] > 2 || data[12] < 1 || data[12] > 60 ||
       sum != (uint16_t(data[13]) | uint16_t(data[14]) << 8)) { return false; }
    *settings = settingsDefaults();
    settings->temperature = TemperatureUnit(data[9]);
    settings->pressure = PressureUnit(data[10]);
    settings->intervalMinutes = data[12];
    return true;
  }
  AppSettings proposed;
  memcpy(&proposed, data + 6, sizeof(proposed));
  if(!settingsValid(proposed)) { return false; }
  *settings = proposed; return true;
}
bool storageSaveSettings(const AppSettings& settings) {
  if(!settingsValid(settings) || !eepromBootstrap()) { return false; }
  uint8_t data[SETTINGS_BYTES] = {};
  write32(data, SETTINGS_TAG); data[4] = 3; data[5] = 7;
  memcpy(data + 6, &settings, sizeof(settings));
  write32(data + 28, crc(data, 28));
  EEPROM.put(EEPROM_ADDR_SETTINGS_BASE, data);
  for(size_t i = 0; i < sizeof(data); ++i) {
    if(EEPROM.read(EEPROM_ADDR_SETTINGS_BASE + i) != data[i]) { return false; }
  }
  return true;
}
uint16_t storageLoadCalibration(uint8_t* data, size_t size, uint32_t profile) {
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
  return bsecStateLoad(data, size, profile);
#else
  (void)data; (void)size; (void)profile; return 0;
#endif
}
bool storageSaveCalibration(const uint8_t* data, uint16_t size, uint32_t profile) {
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
  return bsecStateSave(data, size, profile);
#else
  (void)data; (void)size; (void)profile; return false;
#endif
}
bool storageNonceBegin() { return nonceStoreBegin(); }
bool storageNonceLoad(uint8_t* data, size_t capacity) { return nonceStoreLoad(data, capacity); }
bool storageNonceSave(const uint8_t* data) { return nonceStoreSave(data); }
bool storageFactoryErase() {
  if(!eepromBootstrap()) { return false; }
  // Wipe all legacy/settings/calibration bytes first, then both nonce pages.
  // The caller retries this idempotent operation a bounded number of times.
  uint8_t erased[EEPROM_TOTAL_BYTES]; memset(erased, 255, sizeof(erased));
  EEPROM.put(0, erased);
  for(size_t i = 0; i < sizeof(erased); ++i) { if(EEPROM.read(i) != 255) { return false; } }
  if(!nonceStoreErase()) { return false; }
  storageClearHistory(); return true;
}
