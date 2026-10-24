#include "app_service.h"
#include "settings_defaults.h"
#include "clock_service.h"
#include "storage_service.h"
#include "sensor_service.h"
#include "scheduler_service.h"
#include "encoder_service.h"
#include "lorawan_service.h"
#include "log_service.h"
#include "config.h"
#include <Arduino.h>
#include <mbed.h>
#include <string.h>

namespace {
enum class ManagerState : uint8_t { Starting, Running, Stopping, Erasing, Fault };
ManagerState state = ManagerState::Starting;
AppSettings active = settingsDefaults();
constexpr unsigned QUEUE_SIZE = 8, CACHE_SIZE = 8;
ControlReply replies[QUEUE_SIZE];
unsigned replyHead = 0, replyCount = 0, cacheNext = 0;
struct CachedCommand { bool used; uint8_t bytes[19]; uint8_t size; ControlReply reply; };
CachedCommand cache[CACHE_SIZE] = {};
bool resetPending = false;
uint32_t resetStarted = 0, resetCode = 0, stopStarted = 0;
uint16_t resetRequestId = 0;
uint8_t eraseAttempts = 0;
uint32_t nextTick = 0, uptimeTick = 0, faultLogAt = 0;
uint64_t uptimeMs = 0;
constexpr uint32_t RESET_WINDOW_MS = 3600000UL;

void managerFault(const char* reason) {
  LOG_ERROR("Core", "%s", reason);
  (void)reason;
  lwServiceStop(); state = ManagerState::Fault; faultLogAt = millis();
}
void updateUptime() {
  const uint32_t now = millis(); uptimeMs += uint32_t(now - uptimeTick); uptimeTick = now;
}
NodeStatus nodeStatus() {
  NodeStatus result = {};
  result.uptimeSeconds = uint32_t(uptimeMs / 1000ULL);
  result.ringCount = uint16_t(storageCount());
  result.reportIntervalSeconds = uint16_t(schedulerIntervalMs() / 1000UL);
  result.radioState = uint8_t(lwServiceState());
  result.managerState = uint8_t(state) | (resetPending ? 0x80 : 0);
  result.radioError = lwServiceError();
  result.sampleAgeSeconds = UINT16_MAX;
  StoredReading last = {};
  if(storageRead(0, &last)) {
    const uint32_t age = millis() - last.acquiredMs;
    result.sampleAgeSeconds = age / 1000UL < UINT16_MAX ? uint16_t(age / 1000UL) : UINT16_MAX;
    if(age <= STORAGE_FRESH_MS) { result.validMask = sensorValidMask(last.sample); result.quality = sensorQualityByte(last.sample); }
  }
  return result;
}
void enqueue(const ControlReply& reply) {
  if(replyCount < QUEUE_SIZE) { replies[(replyHead + replyCount++) % QUEUE_SIZE] = reply; }
}
void expireReset() {
  if(!resetPending || uint32_t(millis() - resetStarted) < RESET_WINDOW_MS) { return; }
  resetPending = false; resetCode = 0;
  for(unsigned i = 0; i < replyCount; ++i) {
    auto& reply = replies[(replyHead + i) % QUEUE_SIZE];
    if(reply.kind == ReplyKind::ResetChallenge && reply.requestId == resetRequestId) {
      reply.kind = ReplyKind::Result; reply.result = CommandResult::Expired; reply.value = 0;
    }
  }
  LOG_INFO("Core", "Unconfirmed factory reset expired; normal reporting continues.");
}
AppSettings proposedSettings(const DecodedCommand& command) {
  AppSettings proposed = active;
  if(command.changedMask & 1) { proposed.mode = ReportMode(command.values[0]); }
  if(command.changedMask & 2) { proposed.intervalMinutes = command.values[1]; }
  if(command.changedMask & 4) { proposed.temperature = TemperatureUnit(command.values[2]); }
  if(command.changedMask & 8) { proposed.pressure = PressureUnit(command.values[3]); }
  if(command.changedMask & 16) { proposed.gas = GasUnit(command.values[4]); }
  if(command.changedMask & 32) { proposed.voc = VocUnit(command.values[5]); }
  if(command.changedMask & 64) { proposed.serialLevel = command.values[6]; }
  return proposed;
}
void acceptSettings(const AppSettings& proposed, ControlReply& reply) {
  if(!settingsValid(proposed) || proposed.serialLevel > FW_LOG_LEVEL) { reply.result = CommandResult::Invalid; return; }
  // Validate/save/verify first. No active setting changes until the entire
  // operation succeeds; storage preserves calibration and nonce records.
  if(!storageSaveSettings(proposed)) { reply.result = CommandResult::SaveFailed; return; }
  const AppSettings before = active;
  active = proposed; logSetLevel(LogLevel(active.serialLevel));
  schedulerSettingsChanged(before, active);
  reply.settings = active; reply.kind = ReplyKind::Configuration;
}
void receiveCommand(const uint8_t* bytes, size_t size) {
  if(state != ManagerState::Running || !bytes || !size) { return; }
  expireReset();
  if(replyCount == QUEUE_SIZE) {
    // No command side effects without room for its result. A caller can retry
    // the same request after delivery drains the bounded response queue.
    LOG_WARN("Core", "Control queue full; command not applied."); return;
  }
  DecodedCommand command = {};
  if(!encoderDecode(bytes, size, &command)) {
    if(size >= 4 && bytes[0] == CONTROL_DOWNLINK_VERSION) {
      ControlReply invalid = {}; invalid.command = CommandType(bytes[1]);
      invalid.requestId = uint16_t(bytes[2]) << 8 | bytes[3]; invalid.result = CommandResult::Invalid;
      enqueue(invalid);
    }
    return;
  }
  for(const auto& previous : cache) {
    if(!previous.used || previous.reply.requestId != command.requestId) { continue; }
    ControlReply reply = previous.reply;
    if(previous.size != size || memcmp(previous.bytes, bytes, size)) {
      reply = {}; reply.requestId = command.requestId; reply.command = command.type; reply.result = CommandResult::Conflict;
    } else if(reply.kind == ReplyKind::ResetChallenge &&
              (!resetPending || resetRequestId != reply.requestId || resetCode != reply.value)) {
      reply.kind = ReplyKind::Result; reply.result = CommandResult::Expired; reply.value = 0;
    }
    enqueue(reply); return;
  }
  ControlReply reply = {}; reply.requestId = command.requestId; reply.command = command.type;
  switch(command.type) {
    case CommandType::SetBatch: acceptSettings(proposedSettings(command), reply); break;
    case CommandType::GetConfiguration: reply.kind = ReplyKind::Configuration; reply.settings = active; break;
    case CommandType::GetStatus: reply.kind = ReplyKind::Status; reply.status = nodeStatus(); break;
    case CommandType::ResetSettings: acceptSettings(settingsDefaults(), reply); break;
    case CommandType::RequestReset:
      if(resetPending) { reply.result = CommandResult::Busy; break; }
      resetStarted = millis(); resetCode = lwGenerateResetCode(); resetRequestId = command.requestId;
      resetPending = true; reply.kind = ReplyKind::ResetChallenge; reply.value = resetCode;
      LOG_WARN("Core", "Factory reset requested; confirmation expires in one hour. Normal reports continue.");
      break;
    case CommandType::ConfirmReset:
      if(!resetPending) { reply.result = CommandResult::Expired; }
      else if(command.confirmationCode != resetCode) { reply.result = CommandResult::WrongCode; }
      else {
        // A confirmed reset does not wait indefinitely for another RF reply.
        lwServiceStop(); stopStarted = millis(); state = ManagerState::Stopping;
        LOG_WARN("Core", "Factory reset confirmed; stopping acquisition before erase.");
        return;
      }
      break;
  }
  auto& saved = cache[cacheNext]; saved = {}; saved.used = true; saved.size = uint8_t(size);
  memcpy(saved.bytes, bytes, size); saved.reply = reply; cacheNext = (cacheNext + 1) % CACHE_SIZE;
  enqueue(reply);
}
void resetStep() {
  if(state == ManagerState::Stopping) {
    if(sensorStop()) { state = ManagerState::Erasing; eraseAttempts = 0; }
    else if(uint32_t(millis() - stopStarted) >= 10000UL) { managerFault("Sensor did not stop; factory erase was not started."); }
    return;
  }
  if(state != ManagerState::Erasing) { return; }
  if(!storageFactoryErase()) {
    if(++eraseAttempts >= 3) { managerFault("Factory erase verification failed; radio halted until serviced/restarted."); }
    return;
  }
  resetPending = false; resetCode = 0; replyCount = 0;
  memset(cache, 0, sizeof(cache)); active = settingsDefaults();
  LOG_WARN("Core", "All designated stores erased; restarting with factory defaults.");
  logServiceDelay(100);
  NVIC_SystemReset();
  state = ManagerState::Fault; // Host fakes may return; hardware reset does not.
}
}
const AppSettings& appSettings() { return active; }
void appSetup() {
  logBegin(SERIAL_BAUD_RATE); logSetLevel(LogLevel::INFO); logServiceDelay(2000);
  LOG_INFO("Core", "Garage environmental monitor starting.");
  uptimeTick = millis(); uptimeMs = uptimeTick;
  if(!clockBegin()) { managerFault("Crystal-disciplined timing verification failed."); return; }
  if(!storageBegin()) { managerFault("Storage initialization failed."); return; }
  active = settingsDefaults();
  if(!storageLoadSettings(&active)) { LOG_INFO("Core", "Using factory application defaults."); }
  logSetLevel(LogLevel(active.serialLevel));
  schedulerStart(); sensorStart(); lwServiceStart();
  state = ManagerState::Running; nextTick = millis();
}
void appLoop() {
  logPump();
  const uint32_t now = millis();
  const int32_t remaining = int32_t(nextTick - now);
  if(remaining > 0 && uint32_t(remaining) <= 50) { delay(uint32_t(remaining)); }
  nextTick = millis() + 50; updateUptime();
  if(state == ManagerState::Stopping || state == ManagerState::Erasing) { resetStep(); return; }
  if(state == ManagerState::Fault) {
    if(uint32_t(millis() - faultLogAt) >= 30000UL) {
      LOG_ERROR("Core", "Management fault; radio remains stopped."); faultLogAt = millis();
    }
    return;
  }
  if(state != ManagerState::Running) { return; }
  sensorPoll(active); lwServicePoll(); expireReset();
  DeliveryEvent event = {};
  const ControlReply* pending = replyCount ? &replies[replyHead] : nullptr;
  if(!schedulerPoll(active, pending, nodeStatus(), &event)) { return; }
  if(event.kind == ReportKind::Control && event.transaction.applicationSent && replyCount) {
    replyHead = (replyHead + 1) % QUEUE_SIZE; --replyCount;
  }
  if(event.transaction.downlinkSize) { receiveCommand(event.transaction.downlink, event.transaction.downlinkSize); }
}
