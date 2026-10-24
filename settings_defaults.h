#ifndef SETTINGS_DEFAULTS_H
#define SETTINGS_DEFAULTS_H

#include "common.h"

// Factory application preferences. Provisioning and board/radio constants stay
// in config.h. Valid saved application settings take precedence over defaults.
inline AppSettings settingsDefaults() {
  return {ReportMode::Dynamic, 6, TemperatureUnit::Fahrenheit,
          PressureUnit::InHg, GasUnit::Ohms, VocUnit::Ppb, 3};
}

#endif
