#include "storage_service.h"
#include "settings_defaults.h"
#include "log_service.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <FlashIAPBlockDevice.h>
#include <RadioLib.h>
#include <mbed.h>
#include <string.h>

// Linker-owned symbols must retain global linkage outside private namespaces.
extern uint32_t __etext, __data_start__, __data_end__;

// Private EEPROM bootstrap.

// Total EEPROM reservation
#define EEPROM_TOTAL_BYTES 512

// ------------------------------
// Reserved legacy LoRaWAN block (no longer read or migrated)
// ------------------------------
// Expected bytes:
// - Nonce persistence buffer: RADIOLIB_LORAWAN_NONCES_BUF_SIZE bytes
// - Initialization flag:      1 byte
// Keep this reservation so existing application settings retain their offsets.
// Current LoRaWAN nonce persistence uses the private nonce journal, independently of EEPROM.
#define EEPROM_ADDR_LORAWAN_BASE      0
#define EEPROM_LORAWAN_NONCES_ADDR    EEPROM_ADDR_LORAWAN_BASE
#define EEPROM_LORAWAN_NONCES_BYTES   RADIOLIB_LORAWAN_NONCES_BUF_SIZE
#define EEPROM_LORAWAN_FLAG_ADDR      (EEPROM_LORAWAN_NONCES_ADDR + EEPROM_LORAWAN_NONCES_BYTES)
#define EEPROM_LORAWAN_FLAG_BYTES     1
#define EEPROM_LORAWAN_FLAG_VALUE     0xA5
// Exclusive end of the full LoRaWAN persistence area.
#define EEPROM_ADDR_LORAWAN_END       (EEPROM_LORAWAN_FLAG_ADDR + EEPROM_LORAWAN_FLAG_BYTES)

// ------------------------------
// Application settings block
// ------------------------------
// Expected bytes:
// - Padding to 16-byte boundary: 0..15 bytes (layout hygiene / room for future LoRaWAN growth)
// - Settings reservation:        64 bytes (AppSettings currently uses a small subset)
#define EEPROM_SETTINGS_ALIGN_BYTES    16
#define EEPROM_SETTINGS_PAD_BYTES      ((EEPROM_SETTINGS_ALIGN_BYTES - (EEPROM_ADDR_LORAWAN_END % EEPROM_SETTINGS_ALIGN_BYTES)) % EEPROM_SETTINGS_ALIGN_BYTES)
#define EEPROM_ADDR_SETTINGS_BASE      (EEPROM_ADDR_LORAWAN_END + EEPROM_SETTINGS_PAD_BYTES)
#define EEPROM_SETTINGS_RESERVED_BYTES 64
#define EEPROM_ADDR_SETTINGS_END       (EEPROM_ADDR_SETTINGS_BASE + EEPROM_SETTINGS_RESERVED_BYTES)

// Compile-time guard: fail build if any future layout change would push settings outside
// the configured EEPROM reservation and risk persistence corruption.
#if (EEPROM_ADDR_SETTINGS_END > EEPROM_TOTAL_BYTES)
  #error "EEPROM layout overflow: settings region exceeds EEPROM_TOTAL_BYTES"
#endif

namespace {

bool eepromBootstrap()
{
  static bool isInitialized = false;
  if(isInitialized) {
    return true;
  }

  if(EEPROM.init() != 0) {
    return false;
  }
  EEPROM.setLength(EEPROM_TOTAL_BYTES);
  isInitialized = true;
  return true;
}
} // namespace

// Private nonce journal.
namespace {
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

static bool nonceGeometryReady() {
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
  return true;
}

bool nonceStoreBegin() { return nonceGeometryReady() && nonceStoreScan(); }

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

bool nonceStoreSave(const uint8_t* nonces) {
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

bool nonceStoreLoad(uint8_t* out, size_t capacity) {
  if(!gNonceStoreHasRecord || !out || capacity < sizeof(gNonceSaved)) { return false; }
  memcpy(out, gNonceSaved, sizeof(gNonceSaved));
  return true;
}

// Explicit reset authorization is enforced by management. Ordinary recovery
// never calls this operation and never discards nonce history.
bool nonceStoreErase() {
  if(!nonceGeometryReady()) { return false; }
  for(uint32_t page = 0; page < 2; ++page) {
    if(gNonceFlash.erase(page * NONCE_PAGE_BYTES, NONCE_PAGE_BYTES) != 0) { return false; }
  }
  NonceRecord record;
  for(uint32_t offset = 0; offset < 2 * NONCE_PAGE_BYTES; offset += sizeof(record)) {
    if(gNonceFlash.read(&record, offset, sizeof(record)) != 0 || !nonceErased(record)) { return false; }
  }
  return nonceStoreScan();
}

} // namespace

// Private calibration record.
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
namespace {
namespace CalibrationRecord {
static constexpr size_t BSEC_STATE_MAX_SIZE = 238;
static constexpr size_t STATE_BASE = 128;
static constexpr size_t HEADER_SIZE = 16;
static constexpr size_t RECORD_SIZE = HEADER_SIZE + BSEC_STATE_MAX_SIZE;
// BSEC 2.6.1.0, BME688 3.3 V / 3 s / 4-day profile. Change on compatibility changes.
// Sensor supplies the compatibility tag; storage treats it as opaque metadata.
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
uint16_t bsecStateLoad(uint8_t* state, size_t capacity, uint32_t profile) {
  if(state == nullptr || !eepromBootstrap()) { return 0; }
  uint8_t record[RECORD_SIZE];
  for(size_t i = 0; i < sizeof(record); ++i) { record[i] = EEPROM.read(STATE_BASE + i); }
  const uint16_t size = uint16_t(record[6]) | uint16_t(record[7]) << 8;
  if(read32(record) != 0x32435342UL || record[4] != 1 || record[5] != 0 ||
     !size || size > BSEC_STATE_MAX_SIZE || size > capacity || read32(record + 8) != profile ||
     checksum(record) != read32(record + 12)) { return 0; }
  memcpy(state, record + HEADER_SIZE, size);
  return size;
}
bool bsecStateSave(const uint8_t* state, uint16_t size, uint32_t profile) {
  if(state == nullptr || !size || size > BSEC_STATE_MAX_SIZE || !eepromBootstrap()) { return false; }
  uint8_t record[RECORD_SIZE] = {};
  write32(record, 0x32435342UL);
  record[4] = 1;
  record[6] = uint8_t(size);
  record[7] = uint8_t(size >> 8);
  write32(record + 8, profile);
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

} // namespace CalibrationRecord
} // namespace
#endif

// RAM ring and public storage interface.

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
  return CalibrationRecord::bsecStateLoad(data, size, profile);
#else
  (void)data; (void)size; (void)profile; return 0;
#endif
}
bool storageSaveCalibration(const uint8_t* data, uint16_t size, uint32_t profile) {
#if SENSOR_SERVICE_ENABLED && SENSOR_BSEC_ENABLED
  return CalibrationRecord::bsecStateSave(data, size, profile);
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
