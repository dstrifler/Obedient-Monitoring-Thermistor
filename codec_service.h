#ifndef _CODEC_SERVICE_H
#define _CODEC_SERVICE_H

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "sensor_service.h"
#include "settings.h"

// ============================================================
// DOWNLINK COMMAND TYPES
// ============================================================

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

// ============================================================
// UNIT CONVERSION HELPERS
// ============================================================

static float payloadTempForOutput(float tempC, uint8_t tempUnit) {
  if(tempUnit == TEMP_UNIT_F) {
    return (tempC * 9.0f / 5.0f) + 32.0f;
  }

  return tempC;
}

static float payloadPressureForOutput(float pressurePa, uint8_t pressureUnit) {
  switch(pressureUnit) {
    case PRESSURE_UNIT_HPA:
      return pressurePa / 100.0f;

    case PRESSURE_UNIT_INHG:
      return pressurePa / 3386.389f;

    case PRESSURE_UNIT_PA:
    default:
      return pressurePa;
  }
}

static const char* payloadTempUnitLabel(uint8_t tempUnit) {
  return (tempUnit == TEMP_UNIT_F) ? "F" : "C";
}

static const char* payloadPressureUnitLabel(uint8_t pressureUnit) {
  switch(pressureUnit) {
    case PRESSURE_UNIT_HPA:
      return "hPa";
    case PRESSURE_UNIT_INHG:
      return "inHg";
    case PRESSURE_UNIT_PA:
    default:
      return "Pa";
  }
}

// ============================================================
// BINARY SERIALIZATION HELPERS
// ============================================================

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

// ============================================================
// COMPACT BINARY ENCODER
// ============================================================
//
// Format:
// byte 0  = payload format version (0x01)
// byte 1  = enabled-sensor flags
//           bit0=temp, bit1=humidity, bit2=pressure, bit3=gas
//
// Followed by enabled sensor values in this order:
//
// temp     int16  (C x100 or F x100)
// humidity uint16 (%RH x100)
// pressure uint32 (Pa, hPa x100, or inHg x1000 depending on unit)
// gas      uint32 (ohms)
//
// Note: multibyte integers are encoded in big-endian (network byte order)
// for cross-platform decoding compatibility.
//
// ============================================================

static size_t payloadEncodeCompactBin(
  uint8_t* buffer,
  size_t maxLen,
  const SensorData* data,
  const AppSettings* settings
) {
  if(buffer == nullptr || data == nullptr || settings == nullptr) {
    return 0;
  }

  uint8_t flags = 0;
  size_t needed = 2;

  if(settings->tempEnabled) {
    flags |= 0x01;
    needed += sizeof(int16_t);
  }

  if(settings->humidityEnabled) {
    flags |= 0x02;
    needed += sizeof(uint16_t);
  }

  if(settings->pressureEnabled) {
    flags |= 0x04;
    needed += sizeof(uint32_t);
  }

  if(settings->gasEnabled) {
    flags |= 0x08;
    needed += sizeof(uint32_t);
  }

  if(maxLen < needed) {
    return 0;
  }

  size_t idx = 0;
  buffer[idx++] = 0x01;
  buffer[idx++] = flags;

  if(settings->tempEnabled) {
    float tempOut = payloadTempForOutput(data->temperatureC, settings->tempUnit);
    int16_t tempScaled = (int16_t)lroundf(tempOut * 100.0f);
    payloadWriteU16BE(&buffer[idx], (uint16_t)tempScaled);
    idx += sizeof(tempScaled);
  }

  if(settings->humidityEnabled) {
    uint16_t humScaled = (uint16_t)lroundf(data->humidityPct * 100.0f);
    payloadWriteU16BE(&buffer[idx], humScaled);
    idx += sizeof(humScaled);
  }

  if(settings->pressureEnabled) {
    float pressureOut = payloadPressureForOutput(data->pressurePa, settings->pressureUnit);
    uint32_t pressureScaled = 0;

    switch(settings->pressureUnit) {
      case PRESSURE_UNIT_HPA:
        pressureScaled = (uint32_t)lroundf(pressureOut * 100.0f);
        break;

      case PRESSURE_UNIT_INHG:
        pressureScaled = (uint32_t)lroundf(pressureOut * 1000.0f);
        break;

      case PRESSURE_UNIT_PA:
      default:
        pressureScaled = (uint32_t)lroundf(pressureOut);
        break;
    }

    payloadWriteU32BE(&buffer[idx], pressureScaled);
    idx += sizeof(pressureScaled);
  }

  if(settings->gasEnabled) {
    uint32_t gas = data->gasOhms;
    payloadWriteU32BE(&buffer[idx], gas);
    idx += sizeof(gas);
  }

  return idx;
}

// ============================================================
// STRING / JSON HELPERS
// ============================================================

static size_t payloadAppend(char* buffer, size_t maxLen, size_t idx, const char* text) {
  if(buffer == nullptr || text == nullptr || maxLen == 0) {
    return idx;
  }

  if(idx >= (maxLen - 1)) {
    buffer[maxLen - 1] = '\0';
    return maxLen - 1;
  }

  while(*text != '\0' && idx < (maxLen - 1)) {
    buffer[idx++] = *text++;
  }

  buffer[idx] = '\0';
  return idx;
}

static size_t payloadAppendFormat(char* buffer, size_t maxLen, size_t idx, const char* fmt, ...) {
  if(buffer == nullptr || fmt == nullptr || maxLen == 0 || idx >= maxLen) {
    return idx;
  }

  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(&buffer[idx], maxLen - idx, fmt, args);
  va_end(args);

  if(written < 0) {
    return idx;
  }

  size_t newIdx = idx + (size_t)written;
  if(newIdx >= maxLen) {
    buffer[maxLen - 1] = '\0';
    return maxLen - 1;
  }

  return newIdx;
}

// ============================================================
// COMPACT JSON ENCODER
// ============================================================

static size_t payloadEncodeCompactJson(
  uint8_t* buffer,
  size_t maxLen,
  const SensorData* data,
  const AppSettings* settings
) {
  if(buffer == nullptr || data == nullptr || settings == nullptr || maxLen == 0) {
    return 0;
  }

  char* out = (char*)buffer;
  size_t idx = 0;
  bool first = true;

  out[0] = '\0';
  idx = payloadAppend(out, maxLen, idx, "{");

  if(settings->tempEnabled) {
    float tempOut = payloadTempForOutput(data->temperatureC, settings->tempUnit);
    idx = payloadAppendFormat(
      out,
      maxLen,
      idx,
      "%s\"temperature\":%.2f,\"temp_unit\":\"%s\"",
      first ? "" : ",",
      tempOut,
      payloadTempUnitLabel(settings->tempUnit)
    );
    first = false;
  }

  if(settings->humidityEnabled) {
    idx = payloadAppendFormat(
      out,
      maxLen,
      idx,
      "%s\"humidity\":%.2f",
      first ? "" : ",",
      data->humidityPct
    );
    first = false;
  }

  if(settings->pressureEnabled) {
    float pressureOut = payloadPressureForOutput(data->pressurePa, settings->pressureUnit);
    int precision = (settings->pressureUnit == PRESSURE_UNIT_INHG) ? 3 : 2;

    idx = payloadAppendFormat(
      out,
      maxLen,
      idx,
      "%s\"pressure\":%.*f,\"pressure_unit\":\"%s\"",
      first ? "" : ",",
      precision,
      pressureOut,
      payloadPressureUnitLabel(settings->pressureUnit)
    );
    first = false;
  }

  if(settings->gasEnabled) {
    idx = payloadAppendFormat(
      out,
      maxLen,
      idx,
      "%s\"gas_ohms\":%lu",
      first ? "" : ",",
      (unsigned long)data->gasOhms
    );
    first = false;
  }

  idx = payloadAppend(out, maxLen, idx, "}");
  return strlen(out);
}

// ============================================================
// VERBOSE / DEBUG ENCODER
// ============================================================

static size_t payloadEncodeVerboseDebug(
  uint8_t* buffer,
  size_t maxLen,
  const SensorData* data,
  const AppSettings* settings
) {
  if(buffer == nullptr || data == nullptr || settings == nullptr || maxLen == 0) {
    return 0;
  }

  char* out = (char*)buffer;
  out[0] = '\0';

  float tempOut = payloadTempForOutput(data->temperatureC, settings->tempUnit);
  float pressureOut = payloadPressureForOutput(data->pressurePa, settings->pressureUnit);
  int pressurePrecision = (settings->pressureUnit == PRESSURE_UNIT_INHG) ? 3 : 2;

  int written = snprintf(
    out,
    maxLen,
    "{"
      "\"mode\":\"debug\","
      "\"enabled\":{"
        "\"temp\":%u,"
        "\"humidity\":%u,"
        "\"pressure\":%u,"
        "\"gas\":%u"
      "},"
      "\"readings\":{"
        "\"temperature\":%.2f,"
        "\"temp_unit\":\"%s\","
        "\"humidity\":%.2f,"
        "\"pressure\":%.*f,"
        "\"pressure_unit\":\"%s\","
        "\"gas_ohms\":%lu"
      "},"
      "\"settings\":{"
        "\"report_interval_min\":%u,"
        "\"payload_mode\":%u"
      "}"
    "}",
    settings->tempEnabled,
    settings->humidityEnabled,
    settings->pressureEnabled,
    settings->gasEnabled,
    tempOut,
    payloadTempUnitLabel(settings->tempUnit),
    data->humidityPct,
    pressurePrecision,
    pressureOut,
    payloadPressureUnitLabel(settings->pressureUnit),
    (unsigned long)data->gasOhms,
    settings->reportIntervalMin,
    settings->payloadMode
  );

  if(written < 0 || (size_t)written >= maxLen) {
    return 0;
  }

  return (size_t)written;
}

// ============================================================
// PUBLIC UPLINK ENCODER
// ============================================================

size_t payloadEncodeUplink(
  uint8_t* buffer,
  size_t maxLen,
  const SensorData* data,
  const AppSettings* settings
) {
  if(buffer == nullptr || data == nullptr || settings == nullptr) {
    return 0;
  }

  switch(settings->payloadMode) {
    case PAYLOAD_MODE_COMPACT_JSON:
      return payloadEncodeCompactJson(buffer, maxLen, data, settings);

    case PAYLOAD_MODE_VERBOSE_DEBUG:
      return payloadEncodeVerboseDebug(buffer, maxLen, data, settings);

    case PAYLOAD_MODE_COMPACT_BIN:
    default:
      return payloadEncodeCompactBin(buffer, maxLen, data, settings);
  }
}

// ============================================================
// DOWNLINK DECODER
// ============================================================
//
// Expected format:
// byte 0 = command ID
// byte 1 = value
//
// Examples:
// 01 00 = temp off
// 01 01 = temp on
// 05 01 = temp unit F
// 09 A5 = reset settings
//
// ============================================================

DownlinkCommand payloadDecodeDownlink(const uint8_t* buffer, size_t len) {
  DownlinkCommand cmd;
  cmd.type = CMD_NONE;
  cmd.value = 0;
  cmd.valid = false;

  if(buffer == nullptr || len < 2) {
    return cmd;
  }

  cmd.type = buffer[0];
  cmd.value = buffer[1];

  switch(cmd.type) {
    case CMD_SET_TEMP_ENABLED:
    case CMD_SET_HUMIDITY_ENABLED:
    case CMD_SET_PRESSURE_ENABLED:
    case CMD_SET_GAS_ENABLED:
      cmd.valid = (cmd.value == 0 || cmd.value == 1);
      break;

    case CMD_SET_TEMP_UNIT:
      cmd.valid = (cmd.value == TEMP_UNIT_C || cmd.value == TEMP_UNIT_F);
      break;

    case CMD_SET_PRESSURE_UNIT:
      cmd.valid = (
        cmd.value == PRESSURE_UNIT_HPA ||
        cmd.value == PRESSURE_UNIT_INHG ||
        cmd.value == PRESSURE_UNIT_PA
      );
      break;

    case CMD_SET_REPORT_INTERVAL:
      switch(cmd.value) {
        case 5:
        case 10:
        case 15:
        case 20:
        case 25:
        case 30:
        case 35:
        case 40:
        case 45:
        case 50:
        case 55:
        case 60:
          cmd.valid = true;
          break;

        default:
          cmd.valid = false;
          break;
      }
      break;

    case CMD_SET_PAYLOAD_MODE:
      cmd.valid = (
        cmd.value == PAYLOAD_MODE_COMPACT_BIN ||
        cmd.value == PAYLOAD_MODE_COMPACT_JSON ||
        cmd.value == PAYLOAD_MODE_VERBOSE_DEBUG
      );
      break;

    case CMD_RESET_SETTINGS:
      cmd.valid = (cmd.value == 0xA5);
      break;

    default:
      cmd.valid = false;
      break;
  }

  if(!cmd.valid) {
    cmd.type = CMD_NONE;
    cmd.value = 0;
  }

  return cmd;
}

#endif
