#include "encoder_service.h"
#include "storage_service.h"
#include "log_service.h"
#include <string.h>
#include <math.h>
#include <limits>

namespace {
constexpr size_t ENVIRONMENT_PACKET_SIZE = 11, ENVIRONMENT_REPORT_SIZE = 24;
constexpr uint8_t ENVIRONMENT_PACKET_VERSION = 1;
static void payloadWriteU16BE(uint8_t* dst, uint16_t value) {
  dst[0] = (uint8_t)((value >> 8) & 0xFF);
  dst[1] = (uint8_t)(value & 0xFF);
}

static void payloadWriteU32BE(uint8_t* dst, uint32_t value) {
  dst[0] = (uint8_t)((value >> 24) & 0xFF);
  dst[1] = (uint8_t)((value >> 16) & 0xFF);
  dst[2] = (uint8_t)((value >> 8) & 0xFF);
  dst[3] = (uint8_t)(value & 0xFF);
}

// Fixed 11-byte format, independent of saved field/unit/payload preferences:
// [version:4 | valid T/H/P/G:4], int16 C*100, uint16 RH*100,
// uint16 Pa/2 (rounded), uint32 gas ohms. Invalid slots contain zero.
static size_t payloadEncodeEnvironment(uint8_t* buffer, size_t maxLen, const SensorData* data) {
  if(buffer == nullptr || maxLen < ENVIRONMENT_PACKET_SIZE) { return 0; }
  memset(buffer, 0, ENVIRONMENT_PACKET_SIZE);
  buffer[0] = ENVIRONMENT_PACKET_VERSION << 4;
  if(data == nullptr) { return ENVIRONMENT_PACKET_SIZE; }

  if(isfinite(data->temperatureC) && data->temperatureC >= -40.0f && data->temperatureC <= 85.0f) {
    const int16_t temperature = int16_t(lroundf(data->temperatureC * 100.0f));
    payloadWriteU16BE(buffer + 1, uint16_t(temperature));
    buffer[0] |= 0x01;
  }
  if(isfinite(data->humidityPct) && data->humidityPct >= 0.0f && data->humidityPct <= 100.0f) {
    payloadWriteU16BE(buffer + 3, uint16_t(lroundf(data->humidityPct * 100.0f)));
    buffer[0] |= 0x02;
  }
  if(isfinite(data->pressurePa) && data->pressurePa >= 30000.0f && data->pressurePa <= 110000.0f) {
    payloadWriteU16BE(buffer + 5, uint16_t(lroundf(data->pressurePa / 2.0f)));
    buffer[0] |= 0x04;
  }
  if(data->gasValid && data->gasOhms > 0) {
    payloadWriteU32BE(buffer + 7, data->gasOhms);
    buffer[0] |= 0x08;
  }
  return ENVIRONMENT_PACKET_SIZE;
}

static size_t payloadEncodeReport(uint8_t* buffer, size_t maxLen, const SensorSnapshot* data) {
  if(buffer == nullptr || maxLen < ENVIRONMENT_REPORT_SIZE) { return 0; }
  static_assert(sizeof(float) == sizeof(uint32_t), "Report requires binary32 float");
  static_assert(std::numeric_limits<float>::is_iec559, "Report requires IEEE-754 float");
  memset(buffer, 0, ENVIRONMENT_REPORT_SIZE);
  payloadEncodeEnvironment(buffer, maxLen, data ? &data->environment : nullptr);
  buffer[0] = 0x20 | (buffer[0] & 0x0F);
  if(data == nullptr) { return ENVIRONMENT_REPORT_SIZE; }
  const AirQualityData& air = data->airQuality;
  if((air.validMask & 1) && air.iaqAccuracy <= 3 && isfinite(air.iaq) && air.iaq >= 0 && air.iaq <= 500) {
    payloadWriteU16BE(buffer + 11, uint16_t(lroundf(air.iaq * 10.0f)));
    buffer[19] |= 0x10;
  }
  if((air.validMask & 2) && isfinite(air.co2EquivalentPpm) &&
     air.co2EquivalentPpm >= 0 && air.co2EquivalentPpm <= 65535) {
    payloadWriteU16BE(buffer + 13, uint16_t(lroundf(air.co2EquivalentPpm)));
    buffer[19] |= 0x20;
  }
  if((air.validMask & 4) && isfinite(air.breathVocEquivalentPpm) && air.breathVocEquivalentPpm >= 0) {
    uint32_t bits;
    memcpy(&bits, &air.breathVocEquivalentPpm, sizeof(bits));
    payloadWriteU32BE(buffer + 15, bits);
    buffer[19] |= 0x40;
  }
  if(air.iaqAccuracy <= 3) { buffer[19] |= air.iaqAccuracy; }
  if(air.stabilized) { buffer[19] |= 0x04; }
  if(air.runIn) { buffer[19] |= 0x08; }
  if(data->temperatureHumidityCompensated) { buffer[19] |= 0x80; }
  payloadWriteU32BE(buffer + 20, data->sampleUptimeSeconds);
  return ENVIRONMENT_REPORT_SIZE;
}


} // namespace

bool encoderDecode(const uint8_t* bytes, size_t size, DecodedCommand* command) {
  if(!bytes || !command || size < 4 || bytes[0] != CONTROL_DOWNLINK_VERSION) { return false; }
  DecodedCommand parsed = {};
  parsed.type = CommandType(bytes[1]);
  parsed.requestId = uint16_t(bytes[2]) << 8 | bytes[3];
  if(parsed.type == CommandType::SetBatch) {
    if(size < 7 || bytes[4] < 1 || bytes[4] > 7 || size != size_t(5 + 2 * bytes[4])) { return false; }
    for(unsigned i = 0; i < bytes[4]; ++i) {
      const uint8_t key = bytes[5 + 2 * i];
      if(key < 1 || key > 7 || (parsed.changedMask & (1U << (key - 1)))) { return false; }
      parsed.changedMask |= 1U << (key - 1);
      parsed.values[key - 1] = bytes[6 + 2 * i];
    }
  } else if(parsed.type == CommandType::ConfirmReset || parsed.type == CommandType::ConfirmReboot) {
    if(size != 8) { return false; }
    for(unsigned i = 4; i < 8; ++i) { parsed.confirmationCode = (parsed.confirmationCode << 8) | bytes[i]; }
    if(parsed.confirmationCode < 100000 || parsed.confirmationCode > 999999) { return false; }
  } else if((parsed.type != CommandType::GetConfiguration && parsed.type != CommandType::GetStatus &&
             parsed.type != CommandType::ResetSettings && parsed.type != CommandType::RequestReset &&
             parsed.type != CommandType::RequestReboot) || size != 4) {
    return false;
  }
  *command = parsed; return true;
}
size_t encoderSensorPacket(uint8_t* bytes, size_t capacity,
                          const SensorSnapshot& sample, const AppSettings& settings) {
  if(!bytes || capacity < SENSOR_PACKET_BYTES || !settingsValid(settings)) { return 0; }
  if(payloadEncodeReport(bytes, capacity, &sample) != 24) { return 0; }
  bytes[0] = 0x30 | (bytes[0] & 15);
  bytes[24] = settingsPreferenceByte(settings);
  return SENSOR_PACKET_BYTES;
}
size_t encoderControlPacket(uint8_t* bytes, size_t capacity, const ControlReply& reply) {
  const size_t size = reply.kind == ReplyKind::Configuration ? 17 : reply.kind == ReplyKind::Status ? 26 : 10;
  if(!bytes || capacity < size) { return 0; }
  memset(bytes, 0, size);
  bytes[0] = CONTROL_UPLINK_VERSION; bytes[1] = uint8_t(reply.kind);
  payloadWriteU16BE(bytes + 2, reply.requestId);
  bytes[4] = uint8_t(reply.command); bytes[5] = uint8_t(reply.result);
  payloadWriteU32BE(bytes + 6, reply.value);
  if(reply.kind == ReplyKind::Configuration) {
    const auto& s = reply.settings;
    bytes[10] = uint8_t(s.mode); bytes[11] = s.intervalMinutes; bytes[12] = uint8_t(s.temperature);
    bytes[13] = uint8_t(s.pressure); bytes[14] = uint8_t(s.gas); bytes[15] = uint8_t(s.voc); bytes[16] = s.serialLevel;
  } else if(reply.kind == ReplyKind::Status) {
    const auto& s = reply.status;
    payloadWriteU32BE(bytes + 10, s.uptimeSeconds);
    payloadWriteU16BE(bytes + 14, s.ringCount);
    payloadWriteU16BE(bytes + 16, s.reportIntervalSeconds);
    bytes[18] = s.validMask; bytes[19] = s.quality; bytes[20] = s.radioState; bytes[21] = s.managerState;
    payloadWriteU16BE(bytes + 22, uint16_t(s.radioError));
    payloadWriteU16BE(bytes + 24, s.sampleAgeSeconds);
  }
  return size;
}
RadioTransaction encoderTransmit(ReportKind kind, const AppSettings& settings,
                                 const ControlReply* reply, const NodeStatus& status,
                                 bool requireComplete) {
  uint8_t payload[32]; size_t size = 0;
  if(kind == ReportKind::Sensor) {
    SensorSnapshot sample = {};
    if(storageLatestSnapshot(&sample) && (!requireComplete || sensorValidMask(sample) == 0x7F)) {
      size = encoderSensorPacket(payload, sizeof(payload), sample, settings);
    }
  } else if(kind == ReportKind::Health) {
    ControlReply health = {}; health.kind = ReplyKind::Status; health.status = status;
    size = encoderControlPacket(payload, sizeof(payload), health);
  } else if(reply) { size = encoderControlPacket(payload, sizeof(payload), *reply); }
  if(!size) {
    RadioTransaction failed = {}; failed.result = LW_ERR_INVALID_ARGUMENT; return failed;
  }
  LOG_TRACE_HEX("Encoder", payload, size);
  return lwTransmit(payload, size);
}
