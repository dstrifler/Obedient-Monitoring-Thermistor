#include "common.h"
#include <math.h>


bool settingsValid(const AppSettings& s) {
  return uint8_t(s.mode) <= 1 && s.intervalMinutes >= 1 && s.intervalMinutes <= 60 &&
    uint8_t(s.temperature) <= 1 && uint8_t(s.pressure) <= 4 &&
    uint8_t(s.gas) <= 1 && uint8_t(s.voc) <= 1 && s.serialLevel <= 5;
}
bool settingsEqual(const AppSettings& a, const AppSettings& b) {
  return a.mode == b.mode && a.intervalMinutes == b.intervalMinutes &&
    a.temperature == b.temperature && a.pressure == b.pressure &&
    a.gas == b.gas && a.voc == b.voc && a.serialLevel == b.serialLevel;
}
uint8_t settingsPreferenceByte(const AppSettings& s) {
  return uint8_t(s.temperature) | uint8_t(uint8_t(s.gas) << 1) |
    uint8_t(uint8_t(s.voc) << 2) | uint8_t(uint8_t(s.pressure) << 3);
}


uint8_t sensorValidMask(const SensorSnapshot& s) {
  const auto& e = s.environment;
  const auto& a = s.airQuality;
  uint8_t mask = 0;
  if(isfinite(e.temperatureC) && e.temperatureC >= -40 && e.temperatureC <= 85) { mask |= 1; }
  if(isfinite(e.humidityPct) && e.humidityPct >= 0 && e.humidityPct <= 100) { mask |= 2; }
  if(isfinite(e.pressurePa) && e.pressurePa >= 30000 && e.pressurePa <= 110000) { mask |= 4; }
  if(e.gasValid && e.gasOhms > 0) { mask |= 8; }
  if((a.validMask & 1) && a.iaqAccuracy <= 3 && isfinite(a.iaq) && a.iaq >= 0 && a.iaq <= 500) { mask |= 16; }
  if((a.validMask & 2) && isfinite(a.co2EquivalentPpm) && a.co2EquivalentPpm >= 0 && a.co2EquivalentPpm <= 65535) { mask |= 32; }
  if((a.validMask & 4) && isfinite(a.breathVocEquivalentPpm) && a.breathVocEquivalentPpm >= 0) { mask |= 64; }
  return mask;
}
uint8_t sensorQualityByte(const SensorSnapshot& s) {
  const uint8_t valid = sensorValidMask(s);
  return uint8_t((s.airQuality.iaqAccuracy & 3) | (s.airQuality.stabilized ? 4 : 0) |
    (s.airQuality.runIn ? 8 : 0) | ((valid >> 4) << 4) |
    (s.temperatureHumidityCompensated ? 128 : 0));
}
