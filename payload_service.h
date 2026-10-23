#ifndef PAYLOAD_SERVICE_H
#define PAYLOAD_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include "sensor_service.h"
#include "settings_service.h"

// Fixed FPort 1 environmental test format; multibyte values are big-endian.
static constexpr size_t ENVIRONMENT_PACKET_SIZE = 11;
static constexpr uint8_t ENVIRONMENT_PACKET_VERSION = 1;
// Null data produces a full-length packet with all validity flags cleared.
size_t payloadEncodeEnvironment(uint8_t* buffer, size_t maxLen, const SensorData* data);

enum DownlinkCommandType : uint8_t {
  CMD_NONE                 = 0x00,
  CMD_SET_TEMP_ENABLED     = 0x01,
  CMD_SET_HUMIDITY_ENABLED = 0x02,
  CMD_SET_PRESSURE_ENABLED = 0x03,
  CMD_SET_GAS_ENABLED      = 0x04,
  CMD_SET_TEMP_UNIT        = 0x05,
  CMD_SET_PRESSURE_UNIT    = 0x06,
  CMD_SET_REPORT_INTERVAL  = 0x07,
  CMD_SET_PAYLOAD_MODE     = 0x08,
  CMD_RESET_SETTINGS       = 0x09
};

struct DownlinkCommand {
  uint8_t type;
  uint8_t value;
  bool valid;
};

bool payloadModeIsBestEffort(uint8_t mode);
size_t payloadEncodeUplink(uint8_t* buffer, size_t maxLen,
                           const SensorData* data, const AppSettings* settings);
DownlinkCommand payloadDecodeDownlink(const uint8_t* buffer, size_t len);

#endif
