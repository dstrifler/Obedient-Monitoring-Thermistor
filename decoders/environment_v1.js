// Paste this entire script into the console's custom JavaScript decoder.
// Firmware wire contract: ENVIRONMENT_PACKET.md. No Node.js APIs required.
// Helium disables a decoder after a thrown exception. Reject unexpected input
// with a normal error object so subsequent environmental packets still decode.

function Decoder(bytes, port, uplink_info) {
  // uplink_info is optional console metadata; this format needs only bytes/port.
  if (!bytes || typeof bytes !== "object" || typeof bytes.length !== "number") {
    return { error: "Expected a byte array" };
  }
  // Network housekeeping contains no environmental measurements.
  if ((port === 0 || port === 1) && bytes.length === 0) {
    return { mac_only: true };
  }
  if (port !== 1) {
    return { error: "Expected environmental data on FPort 1" };
  }
  for (var i = 0; i < bytes.length; i++) {
    var value = bytes[i];
    if (typeof value !== "number" || value < 0 || value > 255 || value !== Math.floor(value)) {
      return { error: "Invalid byte at offset " + i };
    }
  }
  if (bytes[0] === 0xA1) { return decodeControl(bytes); }
  if (bytes.length !== 11 && bytes.length !== 24 && bytes.length !== 25) {
    return { error: "Expected 11 v1, 24 v2, or 25 v3 environmental bytes" };
  }
  var version = bytes[0] >> 4;
  if ((version !== 1 && version !== 2 && version !== 3) ||
      (version === 1 && bytes.length !== 11) || (version === 2 && bytes.length !== 24) || (version === 3 && bytes.length !== 25)) {
    return { error: "Unsupported environmental packet version: " + version };
  }

  var flags = bytes[0] & 15;
  var temperature = (bytes[1] << 8) | bytes[2];
  if (temperature & 0x8000) {
    temperature -= 65536;
  }
  var pressurePa = ((bytes[5] << 8) | bytes[6]) * 2;
  // Arithmetic preserves uint32 values above JavaScript's signed bitwise range.
  var gasOhms = bytes[7] * 16777216 + bytes[8] * 65536 + bytes[9] * 256 + bytes[10];

  var decoded = {
    format_version: version,
    valid_mask: flags,
    temperature_c: (flags & 1) ? temperature / 100 : null,
    humidity_pct: (flags & 2) ? ((bytes[3] << 8) | bytes[4]) / 100 : null,
    pressure_pa: (flags & 4) ? pressurePa : null,
    pressure_hpa: (flags & 4) ? pressurePa / 100 : null,
    gas_ohms: (flags & 8) ? gasOhms : null
  };
  if (version >= 2) {
    var status = bytes[19];
    var iaq = ((bytes[11] << 8) | bytes[12]) / 10;
    var voc = decodeBinary32(bytes, 15);
    if ((status & 16) && iaq > 500) { return { error: "Invalid IAQ index" }; }
    if ((status & 64) && (voc === null || voc < 0)) { return { error: "Invalid VOC-equivalent value" }; }
    decoded.iaq = (status & 16) ? iaq : null;
    decoded.co2_equivalent_ppm = (status & 32) ? ((bytes[13] << 8) | bytes[14]) : null;
    decoded.breath_voc_equivalent_ppm = (status & 64) ? voc : null;
    decoded.iaq_accuracy = (status & 16) ? status & 3 : null;
    decoded.sensor_stabilized = !!(status & 4);
    decoded.sensor_run_in_complete = !!(status & 8);
    decoded.air_quality_valid_mask = (status >> 4) & 7;
    decoded.temperature_humidity_compensated = !!(status & 128);
    decoded.sample_uptime_s = bytes[20] * 16777216 + bytes[21] * 65536 + bytes[22] * 256 + bytes[23];
  }
  if (version === 3) {
    var preferences = bytes[24];
    var pressure = (preferences >> 3) & 7;
    if ((preferences & 192) || pressure > 4) { return { error: "Invalid presentation preferences" }; }
    var units = ["mbar", "inHg", "Pa", "kPa", "mmHg"];
    var divisors = [100, 3386.389, 1, 1000, 133.3224];
    decoded.temperature_unit = preferences & 1 ? "F" : "C";
    decoded.temperature = decoded.temperature_c === null ? null : preferences & 1 ? decoded.temperature_c * 1.8 + 32 : decoded.temperature_c;
    decoded.pressure_unit = units[pressure];
    decoded.pressure = decoded.pressure_pa === null ? null : decoded.pressure_pa / divisors[pressure];
    decoded.gas_resistance_unit = preferences & 2 ? "kohm" : "ohm";
    decoded.gas_resistance = decoded.gas_ohms === null ? null : decoded.gas_ohms / (preferences & 2 ? 1000 : 1);
    decoded.breath_voc_equivalent_unit = preferences & 4 ? "ppb" : "ppm";
    decoded.breath_voc_equivalent = decoded.breath_voc_equivalent_ppm === null ? null : decoded.breath_voc_equivalent_ppm * (preferences & 4 ? 1000 : 1);
    decoded.packet_type = "sensor";
  }
  return decoded;
}

// ES5 arithmetic decoder: works in Helium without Buffer, DataView or imports.
function decodeBinary32(bytes, offset) {
  var bits = bytes[offset] * 16777216 + bytes[offset + 1] * 65536 + bytes[offset + 2] * 256 + bytes[offset + 3];
  var sign = bits >= 2147483648 ? -1 : 1;
  var exponent = Math.floor(bits / 8388608) % 256;
  var fraction = bits % 8388608;
  if (exponent === 255) { return null; }
  if (exponent === 0) { return sign * fraction * Math.pow(2, -149); }
  return sign * (1 + fraction / 8388608) * Math.pow(2, exponent - 127);
}

// ChirpStack / LoRaWAN Payload Codec API entry point.
function decodeUplink(input) {
  try {
    var decoded = Decoder(input.bytes, input.fPort);
    if (decoded.error) {
      return { errors: [decoded.error] };
    }
    return { data: decoded };
  } catch (error) {
    return { errors: [error.message] };
  }
}


function readU16(bytes, offset) { return bytes[offset] * 256 + bytes[offset + 1]; }
function readU32(bytes, offset) {
  return bytes[offset] * 16777216 + bytes[offset + 1] * 65536 + bytes[offset + 2] * 256 + bytes[offset + 3];
}
function decodeControl(bytes) {
  if (bytes.length < 10 || bytes[1] > 4 || bytes[5] > 6) { return {error: "Invalid control reply"}; }
  var kind = bytes[1], length = kind === 1 ? 17 : kind === 2 ? 26 : 10;
  if (bytes.length !== length) { return {error: "Invalid control reply length"}; }
  var result = {
    packet_type: kind === 2 && bytes[4] === 0 ? "health" : "control",
    control_version: 1, reply_type: ["result", "configuration", "status", "reset_challenge", "reboot_challenge"][kind],
    request_id: readU16(bytes, 2), command: bytes[4],
    result: ["accepted", "invalid", "save_failed", "busy", "expired", "wrong_code", "conflict"][bytes[5]],
    value: readU32(bytes, 6)
  };
  if (kind === 1) {
    if (bytes[10] > 1 || bytes[11] < 1 || bytes[11] > 60 || bytes[12] > 1 ||
        bytes[13] > 4 || bytes[14] > 1 || bytes[15] > 1 || bytes[16] > 5) {
      return {error: "Invalid configuration reply"};
    }
    result.settings = {
      report_mode: bytes[10] ? "fixed" : "dynamic", interval_minutes: bytes[11],
      temperature_unit: bytes[12] ? "F" : "C", pressure_unit: ["mbar", "inHg", "Pa", "kPa", "mmHg"][bytes[13]],
      gas_unit: bytes[14] ? "kohm" : "ohm", voc_unit: bytes[15] ? "ppb" : "ppm",
      serial_level: ["OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"][bytes[16]]
    };
  } else if (kind === 2) {
    var radioError = readU16(bytes, 22);
    result.status = {
      uptime_s: readU32(bytes, 10), ring_count: readU16(bytes, 14),
      report_interval_s: readU16(bytes, 16), valid_mask: bytes[18],
      iaq_accuracy: bytes[19] & 3, sensor_stabilized: !!(bytes[19] & 4),
      sensor_run_in_complete: !!(bytes[19] & 8), radio_state: bytes[20],
      manager_state: bytes[21] & 63, reset_confirmation_pending: !!(bytes[21] & 128),
      reboot_confirmation_pending: !!(bytes[21] & 64),
      radio_error: radioError & 32768 ? radioError - 65536 : radioError,
      sample_age_s: readU16(bytes, 24) === 65535 ? null : readU16(bytes, 24)
    };
  } else if (kind === 3 || kind === 4) {
    if (bytes[4] !== (kind === 3 ? 5 : 7) || result.value < 100000 || result.value > 999999) {
      return {error: "Invalid confirmation challenge"};
    }
    if (kind === 3) { result.reset_confirmation_code = String(result.value); }
    else { result.reboot_confirmation_code = String(result.value); }
  }
  return result;
}

// Optional LoRaWAN Payload Codec downlink helper. Helium's uplink decoder does
// not call this; use tools/downlink.py to generate console Base64 payloads.
function encodeDownlink(input) {
  try {
    var d = input.data, ids = {set: 1, configuration: 2, status: 3, defaults: 4, reset_request: 5, reset_confirm: 6,
                             reboot_request: 7, reboot_confirm: 8};
    if (!d || !Object.prototype.hasOwnProperty.call(ids, d.command) ||
        typeof d.request_id !== "number" || d.request_id < 0 || d.request_id > 65535 || d.request_id !== Math.floor(d.request_id)) {
      return {errors: ["Provide a command and integer request_id (0..65535)"]};
    }
    var bytes = [193, ids[d.command], Math.floor(d.request_id / 256), d.request_id % 256];
    if (d.command === "set") {
      var keys = ["report_mode", "interval_minutes", "temperature_unit", "pressure_unit", "gas_unit", "voc_unit", "serial_level"];
      var choices = [{dynamic: 0, fixed: 1}, null, {C: 0, F: 1}, {hPa: 0, mbar: 0, mb: 0, inHg: 1, Pa: 2, kPa: 3, mmHg: 4},
                     {ohm: 0, kohm: 1}, {ppm: 0, ppb: 1}, {OFF: 0, ERROR: 1, WARN: 2, INFO: 3, DEBUG: 4, TRACE: 5}];
      var settings = d.settings || {}, count = 0;
      bytes.push(0);
      for (var key in settings) {
        if (!Object.prototype.hasOwnProperty.call(settings, key)) { continue; }
        var index = keys.indexOf(key), value = settings[key];
        if (index < 0) { return {errors: ["Unknown setting: " + key]}; }
        if (index === 1) {
          if (typeof value !== "number" || value < 1 || value > 60 || value !== Math.floor(value)) { return {errors: ["Invalid interval"]}; }
        } else {
          if (!Object.prototype.hasOwnProperty.call(choices[index], value)) { return {errors: ["Invalid setting: " + key]}; }
          value = choices[index][value];
        }
        bytes.push(index + 1, value); count++;
      }
      if (!count) { return {errors: ["Empty settings batch"]}; }
      bytes[4] = count;
    } else if (d.command === "reset_confirm" || d.command === "reboot_confirm") {
      var code = Number(d.code);
      if (!isFinite(code) || code !== Math.floor(code) || code < 100000 || code > 999999) { return {errors: ["Invalid six-digit code"]}; }
      bytes.push(Math.floor(code / 16777216), Math.floor(code / 65536) % 256, Math.floor(code / 256) % 256, code % 256);
    }
    return {bytes: bytes, fPort: 1};
  } catch (error) { return {errors: [error.message]}; }
}
