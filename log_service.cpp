#include "log_service.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if FW_LOG_LEVEL != FW_LOG_LEVEL_OFF
namespace {
constexpr uint8_t LOG_SLOTS = 12;
constexpr uint8_t LOG_RESERVED_SLOTS = 3;
constexpr size_t LOG_LINE_BYTES = 160;
constexpr uint8_t LOG_PUMP_BYTES = 64;
constexpr uint32_t LOG_PUMP_US = 1000;
struct LogRecord {
  char text[LOG_LINE_BYTES];
  uint16_t length;
  uint16_t sent;
  LogLevel level;
};
LogRecord gLogQueue[LOG_SLOTS];
uint8_t gLogHead = 0, gLogCount = 0;
uint32_t gLogDropped[6] = {};
bool gLogReady = false, gLogDropNoticePending = false;
LogLevel gRuntimeLevel = LogLevel(FW_LOG_LEVEL < 3 ? FW_LOG_LEVEL : 3);
LogState gLogState = LogState::Stopped;
bool gLogBackpressured = false;

void logUpdateState() {
  if(!gLogReady) { gLogState = LogState::Stopped; }
  else if(gRuntimeLevel == LogLevel::OFF) { gLogState = LogState::Disabled; }
  else if(gLogCount || gLogDropNoticePending) {
    gLogState = gLogBackpressured ? LogState::Backpressured : LogState::Queued;
  } else { gLogState = LogState::Idle; gLogBackpressured = false; }
}

bool logEnabled(LogLevel level) {
  unsigned value = static_cast<unsigned>(level);
  return value >= FW_LOG_LEVEL_ERROR && value <= FW_LOG_LEVEL && value <= unsigned(gRuntimeLevel);
}
LogRecord& logAt(uint8_t index) { return gLogQueue[(gLogHead + index) % LOG_SLOTS]; }
void logDropped(LogLevel level) {
  uint32_t& count = gLogDropped[static_cast<unsigned>(level)];
  if(count != UINT32_MAX) { ++count; }
  gLogDropNoticePending = true;
  logUpdateState();
}
bool logMakeRoom(LogLevel level) {
  uint8_t limit = level <= LogLevel::WARN ? LOG_SLOTS : LOG_SLOTS - LOG_RESERVED_SLOTS;
  if(gLogCount < limit) { return true; }
  int victim = -1;
  for(uint8_t i = 0; i < gLogCount; ++i) {
    const LogRecord& record = logAt(i);
    // Never evict a partly transmitted line. Prefer the least severe queued
    // record, preserving FIFO order among everything retained.
    if(record.sent == 0 && record.level > level &&
       (victim < 0 || record.level > logAt(static_cast<uint8_t>(victim)).level)) { victim = i; }
  }
  if(victim < 0) { logDropped(level); return false; }
  logDropped(logAt(static_cast<uint8_t>(victim)).level);
  for(uint8_t i = static_cast<uint8_t>(victim); i + 1 < gLogCount; ++i) { logAt(i) = logAt(i + 1); }
  --gLogCount;
  return true;
}
void logDropNotice() {
  // Only add a notice when the queue is empty: it never evicts application
  // messages or recursively logs its own overflow. Counts are cumulative.
#if FW_LOG_LEVEL >= FW_LOG_LEVEL_WARN
  constexpr LogLevel level = LogLevel::WARN;
#else
  constexpr LogLevel level = LogLevel::ERROR;
#endif
  if(!logEnabled(level)) { gLogDropNoticePending = false; return; }
  logWrite(level, "Logger", "Dropped totals: ERROR=%lu WARN=%lu INFO=%lu DEBUG=%lu TRACE=%lu",
    static_cast<unsigned long>(gLogDropped[1]), static_cast<unsigned long>(gLogDropped[2]),
    static_cast<unsigned long>(gLogDropped[3]), static_cast<unsigned long>(gLogDropped[4]),
    static_cast<unsigned long>(gLogDropped[5]));
  gLogDropNoticePending = false;
}
} // namespace

void logSetLevel(LogLevel level) {
  if(unsigned(level) <= 5) {
    gRuntimeLevel = LogLevel(unsigned(level) < FW_LOG_LEVEL ? unsigned(level) : FW_LOG_LEVEL);
    if(gRuntimeLevel == LogLevel::OFF) {
      gLogHead = gLogCount = 0; gLogDropNoticePending = false; gLogBackpressured = false;
    }
    logUpdateState();
  }
}
LogLevel logGetLevel() { return gRuntimeLevel; }
LogState logState() { return gLogState; }

void logBegin(unsigned long baud) {
  if(gLogReady) { return; }
  Serial.begin(baud);
  Serial.set_blocking(false);
  gLogReady = true;
  logUpdateState();
}

void logWrite(LogLevel level, const char* module, const char* format, ...) {
  if(!logEnabled(level) || !format || !logMakeRoom(level)) { return; }
  LogRecord& record = logAt(gLogCount);
  static const char* const names[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
  int prefix = snprintf(record.text, sizeof(record.text), "[%s][%.16s] ",
    names[static_cast<unsigned>(level)], module ? module : "App");
  size_t available = sizeof(record.text) - static_cast<size_t>(prefix) - 2;
  va_list args;
  va_start(args, format);
  int length = vsnprintf(record.text + prefix, available, format, args);
  va_end(args);
  if(length < 0) { snprintf(record.text + prefix, available, "<format error>"); }
  size_t used = strlen(record.text);
  if(length >= static_cast<int>(available)) { memcpy(record.text + used - 3, "...", 3); }
  for(size_t i = 0; i < used; ++i) {
    if(record.text[i] == '\r' || record.text[i] == '\n') { record.text[i] = ' '; }
  }
  record.text[used++] = '\r';
  record.text[used++] = '\n';
  record.text[used] = '\0';
  record.length = static_cast<uint16_t>(used);
  record.sent = 0;
  record.level = level;
  ++gLogCount;
  logUpdateState();
}

void logPump() {
  if(!gLogReady || (gLogCount == 0 && !gLogDropNoticePending)) { return; }
  uint32_t started = micros();
  for(uint8_t emitted = 0; emitted < LOG_PUMP_BYTES && uint32_t(micros() - started) < LOG_PUMP_US; ++emitted) {
    if(gLogCount == 0) {
      if(!gLogDropNoticePending) { break; }
      logDropNotice();
      // The overflow notice may be filtered by runtime verbosity. An empty
      // queue has no record to emit; remain idle instead of sending stale data.
      if(gLogCount == 0) { break; }
    }
    // Apollo3 UART::write() itself waits for readiness even in nonblocking
    // mode. Check before each single-byte write; no other writer may race us.
    if(!Serial.writeable()) { gLogBackpressured = true; break; }
    gLogBackpressured = false;
    LogRecord& record = logAt(0);
    if(Serial.write(static_cast<uint8_t>(record.text[record.sent])) != 1) { gLogBackpressured = true; break; }
    if(++record.sent == record.length) { gLogHead = (gLogHead + 1) % LOG_SLOTS; --gLogCount; }
  }
  logUpdateState();
}

uint32_t logDroppedCount(LogLevel level) {
  unsigned index = static_cast<unsigned>(level);
  return index < 6 ? gLogDropped[index] : 0;
}

void logHex(LogLevel level, const char* module, const uint8_t* data, size_t len) {
  if(!logEnabled(level) || !data) { return; }
  const size_t shown = len > 256 ? 256 : len;
  static const char hex[] = "0123456789ABCDEF";
  for(size_t offset = 0; offset < shown; offset += 32) {
    char row[65];
    size_t bytes = shown - offset > 32 ? 32 : shown - offset;
    for(size_t i = 0; i < bytes; ++i) {
      row[2*i] = hex[data[offset + i] >> 4]; row[2*i + 1] = hex[data[offset + i] & 15];
    }
    row[2*bytes] = '\0';
    logWrite(level, module, "%03u: %s", static_cast<unsigned>(offset), row);
  }
  if(shown != len) { logWrite(level, module, "Hex dump truncated after 256 bytes."); }
}
#else
void logSetLevel(LogLevel) {}
LogLevel logGetLevel() { return LogLevel::OFF; }
LogState logState() { return LogState::Disabled; }
void logBegin(unsigned long) {}
void logPump() {}
uint32_t logDroppedCount(LogLevel) { return 0; }
void logWrite(LogLevel, const char*, const char*, ...) {}
void logHex(LogLevel, const char*, const uint8_t*, size_t) {}
#endif

void logServiceDelay(uint32_t waitMs) {
#if FW_LOG_LEVEL != FW_LOG_LEVEL_OFF
  // Fixed iteration count preserves a bounded startup wait even if millis()
  // is faulty. Service output only at safe waits, never during Class A calls.
  while(waitMs > 0) {
    logPump();
    uint32_t slice = waitMs > 10 ? 10 : waitMs;
    delay(slice);
    waitMs -= slice;
  }
  logPump();
#else
  delay(waitMs);
#endif
}
