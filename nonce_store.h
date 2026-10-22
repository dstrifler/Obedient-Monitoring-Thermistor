#ifndef NONCE_STORE_H
#define NONCE_STORE_H

#include <Arduino.h>
#include <FlashIAPBlockDevice.h>
#include <RadioLib.h>
#include <string.h>

// Two erase pages, separate from the legacy EEPROM page at 0xFE000.
// Check the linked image before touching flash; never erase firmware or settings.
static constexpr uint32_t NONCE_FLASH_BASE = 0xFA000;
static constexpr uint32_t NONCE_PAGE_BYTES = 0x2000;
static constexpr uint32_t NONCE_RECORD_BYTES = 64;
static constexpr uint32_t NONCE_MAGIC = 0x4E4F4E31;
static constexpr uint32_t NONCE_COMMITTED = 0x434F4D31;
static_assert(RADIOLIB_LORAWAN_NONCES_BUF_SIZE <= 44, "Nonce record layout needs updating");

struct NonceRecord {
  uint32_t magic;
  uint32_t sequence;
  uint8_t nonces[44];
  uint32_t crc;
  uint32_t reserved;
  uint32_t committed;
};
static_assert(sizeof(NonceRecord) == NONCE_RECORD_BYTES, "Unexpected record padding");
static FlashIAPBlockDevice gNonceFlash(NONCE_FLASH_BASE, 2 * NONCE_PAGE_BYTES);
static int32_t gNonceLatestOffset = -1;
static uint32_t gNonceSequence = 0;
static uint8_t gNonceSaved[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
static bool gNonceStoreHasRecord = false;

static uint32_t nonceCrc(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for(size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for(uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
  }
  return ~crc;
}

static bool nonceRecordBodyValid(const NonceRecord& r) {
  return r.magic == NONCE_MAGIC &&
    r.crc == nonceCrc(reinterpret_cast<const uint8_t*>(&r), offsetof(NonceRecord, crc));
}

static bool nonceRecordValid(const NonceRecord& r) {
  return nonceRecordBodyValid(r) && r.committed == NONCE_COMMITTED;
}

static bool nonceStoreScan() {
  gNonceLatestOffset = -1;
  gNonceSequence = 0;
  gNonceStoreHasRecord = false;
  for(uint32_t offset = 0; offset < 2 * NONCE_PAGE_BYTES; offset += NONCE_RECORD_BYTES) {
    NonceRecord record;
    if(gNonceFlash.read(&record, offset, sizeof(record)) != 0) { return false; }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
    bool erased = true;
    for(size_t i = 0; i < sizeof(record); ++i) { erased &= bytes[i] == 0xFF; }
    if(erased) { continue; }
    // A complete CRC-protected body is a conservative reservation even if
    // its final commit was interrupted or damaged. Never roll back past it.
    // An incomplete/damaged body is ambiguous: stop rather than guess.
    if(!nonceRecordBodyValid(record)) {
      Serial.println(F("[Persistence] Ambiguous journal record; refusing nonce rollback."));
      return false;
    }
    if(record.committed != NONCE_COMMITTED) {
      Serial.println(F("[Persistence] Retaining CRC-valid reservation with an incomplete/damaged commit marker."));
    }
    if(!gNonceStoreHasRecord || static_cast<int32_t>(record.sequence - gNonceSequence) > 0) {
      gNonceSequence = record.sequence;
      gNonceLatestOffset = offset;
      memcpy(gNonceSaved, record.nonces, sizeof(gNonceSaved));
      gNonceStoreHasRecord = true;
    }
  }
  return true;
}

static bool nonceStoreBegin() {
  extern uint32_t __etext, __data_start__, __data_end__;
  uintptr_t imageEnd = reinterpret_cast<uintptr_t>(&__etext) +
    (reinterpret_cast<uintptr_t>(&__data_end__) - reinterpret_cast<uintptr_t>(&__data_start__));
  if(imageEnd > NONCE_FLASH_BASE || gNonceFlash.init() != 0) {
    Serial.println(F("[Persistence] Flash initialization/image overlap failure."));
    return false;
  }
  if(gNonceFlash.get_program_size() != sizeof(uint32_t) ||
     gNonceFlash.get_erase_size() != NONCE_PAGE_BYTES) {
    Serial.println(F("[Persistence] Unsupported flash geometry."));
    return false;
  }
  return nonceStoreScan();
}

static bool nonceStoreSave(const uint8_t* nonces) {
  if(gNonceStoreHasRecord && memcmp(gNonceSaved, nonces, sizeof(gNonceSaved)) == 0) { return true; }
  uint32_t page = gNonceLatestOffset < 0 ? 0 : (gNonceLatestOffset / NONCE_PAGE_BYTES);
  int32_t target = -1;
  for(uint32_t offset = page * NONCE_PAGE_BYTES; offset < (page + 1) * NONCE_PAGE_BYTES; offset += NONCE_RECORD_BYTES) {
    NonceRecord old;
    if(gNonceFlash.read(&old, offset, sizeof(old)) != 0) { return false; }
    bool erased = true;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&old);
    for(size_t i = 0; i < sizeof(old); ++i) { erased &= bytes[i] == 0xFF; }
    if(erased) { target = offset; break; }
  }
  if(target < 0) {
    // The latest committed record remains intact on the other page until the
    // new record has been programmed, committed and read back successfully.
    page = 1 - page;
    if(gNonceFlash.erase(page * NONCE_PAGE_BYTES, NONCE_PAGE_BYTES) != 0) { return false; }
    target = page * NONCE_PAGE_BYTES;
  }
  NonceRecord record;
  memset(&record, 0xFF, sizeof(record));
  record.magic = NONCE_MAGIC;
  record.sequence = gNonceSequence + 1;
  memcpy(record.nonces, nonces, sizeof(gNonceSaved));
  record.crc = nonceCrc(reinterpret_cast<const uint8_t*>(&record), offsetof(NonceRecord, crc));
  if(gNonceFlash.program(&record, target, offsetof(NonceRecord, committed)) != 0) { return false; }
  uint32_t committed = NONCE_COMMITTED;
  if(gNonceFlash.program(&committed, target + offsetof(NonceRecord, committed), sizeof(committed)) != 0) { return false; }
  NonceRecord check;
  if(gNonceFlash.read(&check, target, sizeof(check)) != 0 || !nonceRecordValid(check) ||
     memcmp(check.nonces, nonces, sizeof(gNonceSaved)) != 0 || check.sequence != record.sequence) { return false; }
  gNonceLatestOffset = target;
  gNonceSequence = check.sequence;
  memcpy(gNonceSaved, nonces, sizeof(gNonceSaved));
  gNonceStoreHasRecord = true;
  return true;
}
#endif
