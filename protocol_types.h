#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include "app_settings.h"
#include <stddef.h>
#include <stdint.h>

static constexpr uint8_t CONTROL_DOWNLINK_VERSION = 0xC1;
static constexpr uint8_t CONTROL_UPLINK_VERSION = 0xA1;
static constexpr size_t SENSOR_PACKET_BYTES = 25;
enum class ReportKind : uint8_t { Sensor, Health, Control };
enum class CommandType : uint8_t { SetBatch = 1, GetConfiguration, GetStatus, ResetSettings, RequestReset, ConfirmReset };
enum class ReplyKind : uint8_t { Result, Configuration, Status, ResetChallenge };
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

#endif
