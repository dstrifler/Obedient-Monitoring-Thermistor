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
  if (bytes.length !== 11 && bytes.length !== 24) {
    return { error: "Expected 11 version-1 or 24 version-2 environmental bytes" };
  }
  for (var i = 0; i < bytes.length; i++) {
    var value = bytes[i];
    if (typeof value !== "number" || value < 0 || value > 255 || value !== Math.floor(value)) {
      return { error: "Invalid byte at offset " + i };
    }
  }
  var version = bytes[0] >> 4;
  if ((version !== 1 && version !== 2) ||
      (version === 1 && bytes.length !== 11) || (version === 2 && bytes.length !== 24)) {
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
  if (version === 2) {
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
