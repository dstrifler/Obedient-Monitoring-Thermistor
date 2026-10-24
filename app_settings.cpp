#include "app_settings.h"

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
