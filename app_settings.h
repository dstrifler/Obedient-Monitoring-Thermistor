#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdint.h>

// Canonical sensor values never change units. These preferences affect display.
enum class ReportMode : uint8_t { Dynamic, Fixed };
enum class TemperatureUnit : uint8_t { Celsius, Fahrenheit };
enum class PressureUnit : uint8_t { Hpa, InHg, Pa, Kpa, MmHg };
enum class GasUnit : uint8_t { Ohms, KiloOhms };
enum class VocUnit : uint8_t { Ppm, Ppb };
struct AppSettings {
  ReportMode mode;
  uint8_t intervalMinutes;
  TemperatureUnit temperature;
  PressureUnit pressure;
  GasUnit gas;
  VocUnit voc;
  uint8_t serialLevel;
};
static_assert(sizeof(AppSettings) == 7, "Settings serialization must remain explicit");
bool settingsValid(const AppSettings& settings);
bool settingsEqual(const AppSettings& left, const AppSettings& right);
uint8_t settingsPreferenceByte(const AppSettings& settings);

#endif
