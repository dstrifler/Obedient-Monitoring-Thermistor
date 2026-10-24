#include "scheduler_service.h"
#include "storage_service.h"
#include "encoder_service.h"
#include "log_service.h"
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <math.h>

namespace {
std::atomic<bool> recordPending(false);
ReportMode currentMode = ReportMode::Dynamic;
bool learned = false, firstSensor = false, attempted = false;
uint32_t interval = SCHEDULER_BASE_MS, reportAnchor = 0, healthAnchor = 0;
uint32_t attemptAt = 0, blockedAt = 0, blockedFor = 0, retryFor = SCHEDULER_MIN_MS;
uint32_t lastEvaluation = 0;
bool retryPending = false, haveEvaluation = false, freshInterval = false;
bool availabilityPending = false;

// Statistical working memory is reused, not another measurement history.
// Thirty-second medians suppress isolated samples while preserving sustained
// changes. All durable observations come from the one-hour storage ring.
constexpr unsigned BIN_COUNT = 120, BIN_SAMPLES = 16;
constexpr uint32_t BIN_MS = 30000UL;
float bins[BIN_COUNT][7];
bool valid[BIN_COUNT][7];
float group[7][BIN_SAMPLES], scratch[BIN_COUNT];
unsigned groupCount[7];
const float floors[7] = {0.01f, 0.01f, 2.0f, 0.0001f, 0.1f, 0.001f, 0.001f};
float median(float* values, unsigned count) {
  std::sort(values, values + count);
  return count & 1 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) * 0.5f;
}
float metricValue(const SensorSnapshot& s, unsigned metric) {
  switch(metric) {
    case 0: return s.environment.temperatureC;
    case 1: return s.environment.humidityPct;
    case 2: return s.environment.pressurePa;
    case 3: return log1pf(float(s.environment.gasOhms));
    case 4: return s.airQuality.iaq;
    case 5: return log1pf(s.airQuality.co2EquivalentPpm);
    default: return log1pf(s.airQuality.breathVocEquivalentPpm);
  }
}
void finishBin(unsigned index) {
  for(unsigned metric = 0; metric < 7; ++metric) {
    // Require multiple acquisitions so a new bin's first sample cannot itself
    // swing policy. Sparse fallback backends use the six-minute fallback.
    valid[index][metric] = groupCount[metric] >= 3;
    if(valid[index][metric]) { bins[index][metric] = median(group[metric], groupCount[metric]); }
    groupCount[metric] = 0;
  }
}
uint32_t intervalFromHistory() {
  const uint32_t generation = storageGeneration(), now = millis();
  for(auto& row : valid) { for(bool& flag : row) { flag = false; } }
  for(auto& count : groupCount) { count = 0; }
  const size_t count = storageCount();
  unsigned currentBin = 0;
  for(size_t offset = 0; offset < count; ++offset) {
    StoredReading record = {};
    if(!storageRead(offset, &record)) { break; }
    const unsigned bin = uint32_t(now - record.acquiredMs) / BIN_MS;
    if(bin >= BIN_COUNT) { break; }
    if(bin != currentBin) { finishBin(currentBin); currentBin = bin; }
    const uint8_t mask = sensorValidMask(record.sample);
    for(unsigned m = 0; m < 7; ++m) {
      if((mask & (1U << m)) && groupCount[m] < BIN_SAMPLES) { group[m][groupCount[m]++] = metricValue(record.sample, m); }
    }
  }
  finishBin(currentBin);
  if(generation != storageGeneration()) { recordPending.store(true); return interval; }
  float activity = 0; bool anyLearned = false, availabilityChanged = false;
  for(unsigned m = 0; m < 7; ++m) {
    // Recent 90 seconds are compared with the preceding available history.
    unsigned recent[3], recentCount = 0;
    for(unsigned i = 0; i < 3; ++i) { if(valid[i][m]) { recent[recentCount++] = i; } }
    if(!valid[0][m] && valid[4][m]) { availabilityChanged = true; }
    if(recentCount < 2) { continue; }
    unsigned n = 0;
    for(unsigned i = 4; i + 1 < BIN_COUNT; ++i) {
      if(valid[i][m] && valid[i + 1][m]) { scratch[n++] = bins[i][m] - bins[i + 1][m]; }
    }
    if(n < 6) { continue; }
    const float slope = median(scratch, n);
    for(unsigned i = 0; i < n; ++i) { scratch[i] = fabsf(scratch[i] - slope); }
    const float noise = std::max(floors[m], median(scratch, n) * 1.4826f);
    float changes[2] = {}; unsigned changeCount = 0;
    for(unsigned i = 0; i + 1 < recentCount; ++i) {
      const unsigned distance = recent[i + 1] - recent[i];
      changes[changeCount++] = fabsf(bins[recent[i]][m] - bins[recent[i + 1]][m]) / distance;
    }
    float score = median(changes, changeCount) / noise;
    // A persistent shifted level remains noteworthy even after its slope stops.
    unsigned base = 3;
    while(base < BIN_COUNT && !valid[base][m]) { ++base; }
    if(base < BIN_COUNT) {
      float residuals[3];
      for(unsigned i = 0; i < recentCount; ++i) {
        const float expected = bins[base][m] + slope * float(base - recent[i]);
        residuals[i] = fabsf(bins[recent[i]][m] - expected) / noise;
      }
      score = std::max(score, median(residuals, recentCount));
    }
    activity = std::max(activity, score); anyLearned = true;
  }
  if(availabilityChanged && firstSensor) { availabilityPending = true; }
  if(!anyLearned) { return SCHEDULER_BASE_MS; }
  const float seconds = activity > 0.1f ? 360.0f / activity : 3600.0f;
  const uint32_t target = uint32_t(std::max(60.0f, std::min(3600.0f, seconds)) * 1000);
  return availabilityChanged ? std::min(target, SCHEDULER_BASE_MS) : target;
}
}
void schedulerStart() {
  learned = firstSensor = attempted = retryPending = haveEvaluation = freshInterval = false;
  interval = SCHEDULER_BASE_MS;
  reportAnchor = healthAnchor = millis(); lastEvaluation = reportAnchor;
  blockedAt = blockedFor = attemptAt = 0; retryFor = SCHEDULER_MIN_MS;
  availabilityPending = false; currentMode = ReportMode::Dynamic;
  recordPending.store(false); storageSetCommitNotification(schedulerNotifyRecord);
}
void schedulerNotifyRecord() { recordPending.store(true); }
void schedulerSettingsChanged(const AppSettings& before, const AppSettings& after) {
  currentMode = after.mode;
  if(before.mode != after.mode || before.intervalMinutes != after.intervalMinutes) {
    interval = after.mode == ReportMode::Fixed ? uint32_t(after.intervalMinutes) * 60000UL : SCHEDULER_BASE_MS;
    reportAnchor = healthAnchor = millis(); retryPending = false; freshInterval = true; blockedFor = 0;
    recordPending.store(true); haveEvaluation = false;
  }
}
uint32_t schedulerIntervalMs() {
  SensorSnapshot sample = {};
  if(currentMode == ReportMode::Dynamic &&
     (!firstSensor || !storageLatestSnapshot(&sample) || !sensorValidMask(sample))) { return SCHEDULER_BASE_MS; }
  return interval;
}
bool schedulerLearningComplete() { return learned; }
bool schedulerFirstSensorSent() { return firstSensor; }
bool schedulerPoll(const AppSettings& settings, const ControlReply* control,
                   const NodeStatus& status, DeliveryEvent* event) {
  if(!event) { return false; }
  currentMode = settings.mode;
  const uint32_t now = millis();
  uint32_t firstMs = 0;
  if(!learned && storageFirstReadingMs(&firstMs) && uint32_t(now - firstMs) >= SCHEDULER_BASE_MS) { learned = true; }
  const bool newData = recordPending.exchange(false);
  if(settings.mode == ReportMode::Fixed) { interval = uint32_t(settings.intervalMinutes) * 60000UL; }
  else if(newData) {
    const uint32_t target = intervalFromHistory();
    if(firstSensor) {
      // Bound policy movement by elapsed time, not notification count. One
      // acquisition cannot turn a one-hour interval into one-minute reporting.
      const uint32_t elapsed = haveEvaluation ? uint32_t(now - lastEvaluation) : 0;
      const uint32_t lower = uint32_t(float(interval) * expf(-0.69314718f * float(elapsed) / 60000.0f));
      const uint32_t upper = uint32_t(std::min(uint64_t(SCHEDULER_MAX_MS), uint64_t(interval) + elapsed));
      interval = std::max(SCHEDULER_MIN_MS, std::min(upper, std::max(lower, target)));
      if(availabilityPending) { interval = std::min(interval, SCHEDULER_BASE_MS); }
    }
    lastEvaluation = now; haveEvaluation = true;
  }
  if(!lwServiceReady() || (attempted && uint32_t(now - attemptAt) < SCHEDULER_MIN_MS) ||
     (blockedFor && uint32_t(now - blockedAt) < blockedFor)) { return false; }
  SensorSnapshot sample = {};
  const uint8_t mask = storageLatestSnapshot(&sample) ? sensorValidMask(sample) : 0;
  ReportKind kind;
  if(control) { kind = ReportKind::Control; }
  else if(!firstSensor && learned && mask == 0x7F &&
          ((!freshInterval && settings.mode == ReportMode::Dynamic) || uint32_t(now - reportAnchor) >= interval)) { kind = ReportKind::Sensor; }
  else if(!firstSensor || !mask) {
    const uint32_t healthInterval = settings.mode == ReportMode::Dynamic ? SCHEDULER_BASE_MS : interval;
    if(uint32_t(now - healthAnchor) < healthInterval) { return false; }
    kind = ReportKind::Health;
  } else {
    if(!retryPending && uint32_t(now - reportAnchor) < interval) { return false; }
    kind = ReportKind::Sensor;
  }
  const uint32_t wait = lwServiceWait();
  if(wait) { blockedAt = now; blockedFor = std::min(wait, SCHEDULER_MIN_MS); return false; }
  attempted = true; attemptAt = now;
  event->kind = kind;
  event->transaction = encoderTransmit(kind, settings, control, status, !firstSensor);
  const auto& tx = event->transaction;
  if(tx.applicationSent) {
    retryFor = SCHEDULER_MIN_MS; retryPending = false; blockedFor = 0;
    if(kind == ReportKind::Sensor) {
      firstSensor = true; freshInterval = false; availabilityPending = false; reportAnchor = healthAnchor = now;
      LOG_DEBUG("Scheduler", "Sensor report sent; interval=%lu seconds.", (unsigned long)(interval / 1000));
    } else if(kind == ReportKind::Health) {
      healthAnchor = now;
      // A fixed-mode health packet occupies the regular reporting slot. New
      // sensor data must wait for the next slot rather than create an early TX.
      if(settings.mode == ReportMode::Fixed) { reportAnchor = now; }
    }
  } else {
    blockedAt = millis(); blockedFor = tx.macOnly ? SCHEDULER_MIN_MS : retryFor;
    if(kind == ReportKind::Sensor) { retryPending = true; }
    if(tx.result < 0) { retryFor = std::min(uint32_t(300000), uint32_t(retryFor * 2UL)); }
  }
  return true;
}
