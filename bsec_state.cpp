#include "firmware_features.h"
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
#include "bsec_state.h"
#include "eeprom_service.h"
#include <EEPROM.h>
#include <string.h>

static constexpr size_t STATE_BASE = 128;
static constexpr size_t HEADER_SIZE = 16;
static constexpr size_t RECORD_SIZE = HEADER_SIZE + BSEC_STATE_MAX_SIZE;
// BSEC 2.6.1.0, BME688 3.3 V / 3 s / 4-day profile. Change on compatibility changes.
static constexpr uint32_t PROFILE_ID = 0x26103304UL;
static_assert(STATE_BASE >= EEPROM_ADDR_SETTINGS_END, "Calibration overlaps settings");
static_assert(STATE_BASE + RECORD_SIZE <= EEPROM_TOTAL_BYTES, "Calibration exceeds EEPROM");

static uint32_t crcStep(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for(unsigned bit = 0; bit < 8; ++bit) { crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1))); }
  return crc;
}
static uint32_t checksum(const uint8_t* record) {
  uint32_t crc = UINT32_MAX;
  for(size_t i = 0; i < RECORD_SIZE; ++i) {
    if(i < 12 || i >= HEADER_SIZE) { crc = crcStep(crc, record[i]); }
  }
  return ~crc;
}
static void write32(uint8_t* out, uint32_t value) {
  for(unsigned i = 0; i < 4; ++i) { out[i] = uint8_t(value >> (8 * i)); }
}
static uint32_t read32(const uint8_t* in) {
  return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}
uint16_t bsecStateLoad(uint8_t* state, size_t capacity) {
  if(state == nullptr || !eepromBootstrap()) { return 0; }
  uint8_t record[RECORD_SIZE];
  for(size_t i = 0; i < sizeof(record); ++i) { record[i] = EEPROM.read(STATE_BASE + i); }
  const uint16_t size = uint16_t(record[6]) | uint16_t(record[7]) << 8;
  if(read32(record) != 0x32435342UL || record[4] != 1 || record[5] != 0 ||
     !size || size > BSEC_STATE_MAX_SIZE || size > capacity || read32(record + 8) != PROFILE_ID ||
     checksum(record) != read32(record + 12)) { return 0; }
  memcpy(state, record + HEADER_SIZE, size);
  return size;
}
bool bsecStateSave(const uint8_t* state, uint16_t size) {
  if(state == nullptr || !size || size > BSEC_STATE_MAX_SIZE || !eepromBootstrap()) { return false; }
  uint8_t record[RECORD_SIZE] = {};
  write32(record, 0x32435342UL);
  record[4] = 1;
  record[6] = uint8_t(size);
  record[7] = uint8_t(size >> 8);
  write32(record + 8, PROFILE_ID);
  memcpy(record + HEADER_SIZE, state, size);
  write32(record + 12, checksum(record));
  // One bulk operation: the stock EEPROM implementation preserves the other
  // reserved bytes and performs at most one page erase, rather than one/byte.
  EEPROM.put(STATE_BASE, record);
  for(size_t i = 0; i < sizeof(record); ++i) {
    if(EEPROM.read(STATE_BASE + i) != record[i]) { return false; }
  }
  return true;
}
#endif
