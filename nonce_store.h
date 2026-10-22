#ifndef NONCE_STORE_H
#define NONCE_STORE_H

#include <Arduino.h>
#include <FlashIAPBlockDevice.h>
#include <RadioLib.h>
#include <string.h>
#include "log_service.h"

// Two erase pages, separate from the legacy EEPROM page at 0xFE000.
// Check the linked image before touching flash; never erase firmware or settings.
static constexpr uint32_t NONCE_FLASH_BASE = 0xFA000;
static constexpr uint32_t NONCE_PAGE_BYTES = 0x2000;
static constexpr uint32_t NONCE_RECORD_BYTES = 64;
static constexpr uint32_t NONCE_MAGIC = 0x4E4F4E31;
// Final two slots on the active page authorize reclamation and activation.
static constexpr uint32_t NONCE_DATA_SLOTS = NONCE_PAGE_BYTES / NONCE_RECORD_BYTES - 2;
static constexpr uint32_t NONCE_INTENT_MAGIC = 0x4E494E31;
static constexpr uint32_t NONCE_ACTIVATE_MAGIC = 0x4E414331;
static constexpr uint32_t NONCE_SNAPSHOT_MAGIC = 0x4E534E31;
static constexpr uint32_t NONCE_COMMITTED = 0x434F4D31;
static_assert(RADIOLIB_LORAWAN_NONCES_BUF_SIZE <= 32, "Nonce record layout needs updating");

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
static int32_t gNonceReclaimIntent = -1;
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
  return (r.magic == NONCE_MAGIC || r.magic == NONCE_INTENT_MAGIC ||
          r.magic == NONCE_ACTIVATE_MAGIC || r.magic == NONCE_SNAPSHOT_MAGIC) &&
    r.crc == nonceCrc(reinterpret_cast<const uint8_t*>(&r), offsetof(NonceRecord, crc));
}

static bool nonceRecordValid(const NonceRecord& r) {
  return nonceRecordBodyValid(r) && r.committed == NONCE_COMMITTED;
}

static bool nonceErased(const NonceRecord& record) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  for(size_t i = 0; i < sizeof(record); ++i) { if(bytes[i] != 0xFF) { return false; } }
  return true;
}

// Reclamation metadata is inside the CRC-covered body, after nonce bytes.
static uint32_t nonceMeta(const NonceRecord& record, size_t index) {
  uint32_t value;
  memcpy(&value, record.nonces + 32 + index * sizeof(value), sizeof(value));
  return value;
}

static bool nonceMatchesTransition(const NonceRecord& record, uint32_t magic,
                                   const NonceRecord& intent, uint32_t sequence) {
  return nonceRecordBodyValid(record) && record.magic == magic && record.sequence == sequence &&
    nonceMeta(record, 0) == nonceMeta(intent, 0) && nonceMeta(record, 1) == intent.sequence &&
    memcmp(record.nonces, intent.nonces, sizeof(gNonceSaved)) == 0;
}

static bool nonceStoreScan() {
  gNonceLatestOffset = -1;
  gNonceSequence = 0;
  gNonceStoreHasRecord = false;
  gNonceReclaimIntent = -1;
  NonceRecord intent;
  int32_t intentOffset = -1;
  // First discover the newest intact reclamation authority. Do not reject
  // stale-page debris before learning which page was authorized for erasure.
  for(uint32_t page = 0; page < 2; ++page) {
    uint32_t offset = page * NONCE_PAGE_BYTES + NONCE_DATA_SLOTS * NONCE_RECORD_BYTES;
    NonceRecord candidate;
    if(gNonceFlash.read(&candidate, offset, sizeof(candidate)) != 0) { return false; }
    if(nonceRecordBodyValid(candidate) && candidate.magic == NONCE_INTENT_MAGIC &&
       nonceMeta(candidate, 0) == 1 - page && nonceMeta(candidate, 1) == candidate.sequence &&
       (intentOffset < 0 || static_cast<int32_t>(candidate.sequence - intent.sequence) > 0)) {
      intent = candidate;
      intentOffset = offset;
    }
  }
  int32_t activePage = -1;  // Legacy journal: both pages must be checked.
  bool haveSequence = false;
  if(intentOffset >= 0) {
    uint32_t owner = intentOffset / NONCE_PAGE_BYTES;
    uint32_t target = nonceMeta(intent, 0);
    NonceRecord activation;
    if(gNonceFlash.read(&activation, intentOffset + NONCE_RECORD_BYTES, sizeof(activation)) != 0) { return false; }
    if(nonceErased(activation)) {
      // No transmission on the target page is permitted before activation.
      // Its erase/copy may be interrupted at any bit; retain the owner page.
      activePage = owner;
      gNonceReclaimIntent = intentOffset;
      LOG_WARN("Persistence", "Recovering authorized stale-page reclamation.");
    } else if(nonceMatchesTransition(activation, NONCE_ACTIVATE_MAGIC, intent, intent.sequence + 2)) {
      NonceRecord snapshot;
      if(gNonceFlash.read(&snapshot, target * NONCE_PAGE_BYTES, sizeof(snapshot)) != 0 ||
         !nonceMatchesTransition(snapshot, NONCE_SNAPSHOT_MAGIC, intent, intent.sequence + 1)) {
        LOG_ERROR("Persistence", "Activated page snapshot damaged; no rollback permitted.");
        return false;
      }
      activePage = target;
      gNonceSequence = activation.sequence;
      haveSequence = true;
    } else {
      LOG_ERROR("Persistence", "Ambiguous page activation; explicit recovery required.");
      return false;
    }
  }
  for(uint32_t page = 0; page < 2; ++page) {
    if(activePage >= 0 && page != static_cast<uint32_t>(activePage)) { continue; }
    for(uint32_t slot = 0; slot < NONCE_PAGE_BYTES / NONCE_RECORD_BYTES; ++slot) {
      uint32_t offset = page * NONCE_PAGE_BYTES + slot * NONCE_RECORD_BYTES;
      NonceRecord record;
      if(gNonceFlash.read(&record, offset, sizeof(record)) != 0) { return false; }
      if(nonceErased(record)) { continue; }
      bool dataRecord = record.magic == NONCE_MAGIC || (record.magic == NONCE_SNAPSHOT_MAGIC && slot == 0);
      bool currentIntent = static_cast<int32_t>(offset) == gNonceReclaimIntent;
      if(!nonceRecordBodyValid(record) || (!dataRecord && !currentIntent)) {
        LOG_ERROR("Persistence", "Ambiguous current journal record; refusing nonce rollback.");
        return false;
      }
      bool newer = !haveSequence || static_cast<int32_t>(record.sequence - gNonceSequence) > 0;
      if(!gNonceStoreHasRecord || newer) {
        gNonceLatestOffset = offset;
        memcpy(gNonceSaved, record.nonces, sizeof(gNonceSaved));
        gNonceStoreHasRecord = true;
      }
      if(newer) { gNonceSequence = record.sequence; haveSequence = true; }
    }
  }
  return activePage < 0 || gNonceStoreHasRecord;
}

static bool nonceStoreBegin() {
  extern uint32_t __etext, __data_start__, __data_end__;
  uintptr_t imageEnd = reinterpret_cast<uintptr_t>(&__etext) +
    (reinterpret_cast<uintptr_t>(&__data_end__) - reinterpret_cast<uintptr_t>(&__data_start__));
  if(imageEnd > NONCE_FLASH_BASE || gNonceFlash.init() != 0) {
    LOG_ERROR("Persistence", "Flash initialization/image overlap failure.");
    return false;
  }
  if(gNonceFlash.get_program_size() != sizeof(uint32_t) ||
     gNonceFlash.get_erase_size() != NONCE_PAGE_BYTES) {
    LOG_ERROR("Persistence", "Unsupported flash geometry.");
    return false;
  }
  return nonceStoreScan();
}

static bool nonceWriteRecord(uint32_t offset, uint32_t magic, uint32_t sequence,
                             const uint8_t* nonces, uint32_t target = UINT32_MAX,
                             uint32_t parent = UINT32_MAX) {
  NonceRecord record;
  memset(&record, 0xFF, sizeof(record));
  record.magic = magic;
  record.sequence = sequence;
  memcpy(record.nonces, nonces, sizeof(gNonceSaved));
  if(magic != NONCE_MAGIC) {
    memcpy(record.nonces + 32, &target, sizeof(target));
    memcpy(record.nonces + 36, &parent, sizeof(parent));
  }
  record.crc = nonceCrc(reinterpret_cast<const uint8_t*>(&record), offsetof(NonceRecord, crc));
  if(gNonceFlash.program(&record, offset, offsetof(NonceRecord, committed)) != 0) { return false; }
  record.committed = NONCE_COMMITTED;
  if(gNonceFlash.program(&record.committed, offset + offsetof(NonceRecord, committed), sizeof(record.committed)) != 0) { return false; }
  NonceRecord check;
  return gNonceFlash.read(&check, offset, sizeof(check)) == 0 &&
    nonceRecordValid(check) && memcmp(&check, &record, sizeof(record)) == 0;
}

static bool nonceResumeReclamation() {
  NonceRecord intent;
  if(gNonceReclaimIntent < 0 ||
     gNonceFlash.read(&intent, gNonceReclaimIntent, sizeof(intent)) != 0 ||
     !nonceRecordBodyValid(intent) || intent.magic != NONCE_INTENT_MAGIC) { return false; }
  uint32_t target = nonceMeta(intent, 0);
  uint32_t owner = gNonceReclaimIntent / NONCE_PAGE_BYTES;
  if(target != 1 - owner) { return false; }
  // No target-page data may authorize transmission until the owner page has
  // a verified activation record. Retrying this erase after reboot is safe.
  if(gNonceFlash.erase(target * NONCE_PAGE_BYTES, NONCE_PAGE_BYTES) != 0 ||
     !nonceWriteRecord(target * NONCE_PAGE_BYTES, NONCE_SNAPSHOT_MAGIC, intent.sequence + 1,
                       intent.nonces, target, intent.sequence) ||
     !nonceWriteRecord(gNonceReclaimIntent + NONCE_RECORD_BYTES, NONCE_ACTIVATE_MAGIC,
                       intent.sequence + 2, intent.nonces, target, intent.sequence)) { return false; }
  gNonceLatestOffset = target * NONCE_PAGE_BYTES;
  gNonceSequence = intent.sequence + 2;
  memcpy(gNonceSaved, intent.nonces, sizeof(gNonceSaved));
  gNonceStoreHasRecord = true;
  gNonceReclaimIntent = -1;
  return true;
}

static bool nonceStoreSave(const uint8_t* nonces) {
  if(gNonceReclaimIntent >= 0 && !nonceResumeReclamation()) { return false; }
  if(gNonceStoreHasRecord && memcmp(gNonceSaved, nonces, sizeof(gNonceSaved)) == 0) { return true; }
  uint32_t page = gNonceLatestOffset < 0 ? 0 : (gNonceLatestOffset / NONCE_PAGE_BYTES);
  int32_t target = -1;
  for(uint32_t slot = 0; slot < NONCE_DATA_SLOTS; ++slot) {
    uint32_t offset = page * NONCE_PAGE_BYTES + slot * NONCE_RECORD_BYTES;
    NonceRecord old;
    if(gNonceFlash.read(&old, offset, sizeof(old)) != 0) { return false; }
    if(nonceErased(old)) { target = offset; break; }
  }
  if(target < 0) {
    uint32_t intentOffset = page * NONCE_PAGE_BYTES + NONCE_DATA_SLOTS * NONCE_RECORD_BYTES;
    NonceRecord spare[2];
    if(!gNonceStoreHasRecord || gNonceFlash.read(spare, intentOffset, sizeof(spare)) != 0 ||
       !nonceErased(spare[0]) || !nonceErased(spare[1])) {
      LOG_ERROR("Persistence", "No reclamation slots; legacy full page needs explicit migration.");
      return false;
    }
    uint32_t intentSequence = gNonceSequence + 1;
    if(!nonceWriteRecord(intentOffset, NONCE_INTENT_MAGIC, intentSequence, gNonceSaved,
                         1 - page, intentSequence)) { return false; }
    gNonceReclaimIntent = intentOffset;
    gNonceLatestOffset = intentOffset;
    gNonceSequence = intentSequence;
    if(!nonceResumeReclamation()) { return false; }
    target = gNonceLatestOffset + NONCE_RECORD_BYTES;
  }
  uint32_t next = gNonceSequence + 1;
  if(!nonceWriteRecord(target, NONCE_MAGIC, next, nonces)) { return false; }
  gNonceLatestOffset = target;
  gNonceSequence = next;
  memcpy(gNonceSaved, nonces, sizeof(gNonceSaved));
  gNonceStoreHasRecord = true;
  return true;
}
#endif
