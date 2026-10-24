#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

// Shared contracts and pure helpers only; services own all mutable state/I/O.
// Build features: sensor acquisition feeds Serial and the RAM ring; scheduler selects v3 reports.
// Set to 0 for a radio-only build without the Bosch sensor dependency.
#ifndef SENSOR_SERVICE_ENABLED
#define SENSOR_SERVICE_ENABLED 1
#endif
static_assert(SENSOR_SERVICE_ENABLED == 0 || SENSOR_SERVICE_ENABLED == 1,
              "SENSOR_SERVICE_ENABLED must be 0 or 1");

// Set to 0 for the original forced-mode sensor backend without BSEC.
#ifndef SENSOR_BSEC_ENABLED
#define SENSOR_BSEC_ENABLED 1
#endif
static_assert(SENSOR_BSEC_ENABLED == 0 || SENSOR_BSEC_ENABLED == 1,
              "SENSOR_BSEC_ENABLED must be 0 or 1");

// Application settings.
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

// Coherent sensor acquisition contracts.
struct SensorData {
  float temperatureC;
  float humidityPct;
  float pressurePa;
  uint32_t gasOhms;
  bool gasValid;
};
struct AirQualityData {
  float iaq;
  float co2EquivalentPpm;
  float breathVocEquivalentPpm;
  uint8_t validMask;
  uint8_t iaqAccuracy;
  bool stabilized;
  bool runIn;
};
struct SensorSnapshot {
  SensorData environment;
  AirQualityData airQuality;
  uint32_t sampleUptimeSeconds;
  bool temperatureHumidityCompensated;
};

// Bits 0..6: temperature, humidity, pressure, gas, IAQ, CO2eq, VOCeq.
uint8_t sensorValidMask(const SensorSnapshot& sample);
uint8_t sensorQualityByte(const SensorSnapshot& sample);

// Application wire protocol contracts.
static constexpr uint8_t CONTROL_DOWNLINK_VERSION = 0xC1;
static constexpr uint8_t CONTROL_UPLINK_VERSION = 0xA1;
static constexpr size_t SENSOR_PACKET_BYTES = 25;
enum class ReportKind : uint8_t { Sensor, Health, Control };
enum class CommandType : uint8_t {
  SetBatch = 1, GetConfiguration, GetStatus, ResetSettings,
  RequestReset, ConfirmReset, RequestReboot, ConfirmReboot
};
enum class ReplyKind : uint8_t { Result, Configuration, Status, ResetChallenge, RebootChallenge };
enum class CommandResult : uint8_t { Accepted, Invalid, SaveFailed, Busy, Expired, WrongCode, Conflict };
struct DecodedCommand {
  CommandType type;
  uint16_t requestId;
  uint8_t changedMask;
  uint8_t values[7];
  uint32_t confirmationCode;
};
struct NodeStatus {
  uint32_t uptimeSeconds;
  uint16_t ringCount;
  uint16_t reportIntervalSeconds;
  uint8_t validMask;
  uint8_t quality;
  uint8_t radioState;
  uint8_t managerState;
  int16_t radioError;
  uint16_t sampleAgeSeconds;
};
struct ControlReply {
  ReplyKind kind;
  uint16_t requestId;
  CommandType command;
  CommandResult result;
  uint32_t value;
  AppSettings settings;
  NodeStatus status;
};

// Radio outcomes shared by scheduler, encoder, and management.
enum class RadioState : uint8_t { Stopped, Initializing, Joining, Ready, Cooldown, Rollover, Fault };
struct RadioTransaction {
  int16_t result;
  bool applicationSent;
  bool macOnly;
  size_t downlinkSize;
  uint8_t downlink[255];
};

#endif
