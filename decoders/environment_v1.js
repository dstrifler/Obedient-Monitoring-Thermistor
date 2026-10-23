// Paste this entire script into the console's custom JavaScript decoder.
// Firmware wire contract: ENVIRONMENT_PACKET.md. No Node.js APIs required.

function Decoder(bytes, port, uplink_info) {
  // uplink_info is optional console metadata; this format needs only bytes/port.
  if (!bytes || typeof bytes !== "object" || typeof bytes.length !== "number") {
    throw new Error("Expected a byte array");
  }
  // Network housekeeping contains no environmental measurements.
  if (port === 0 && bytes.length === 0) {
    return { mac_only: true };
  }
  if (port !== 1) {
    throw new Error("Expected environmental data on FPort 1");
  }
  if (bytes.length !== 11) {
    throw new Error("Expected exactly 11 environmental bytes");
  }
  for (var i = 0; i < bytes.length; i++) {
    var value = bytes[i];
    if (typeof value !== "number" || value < 0 || value > 255 || value !== Math.floor(value)) {
      throw new Error("Invalid byte at offset " + i);
    }
  }
  var version = bytes[0] >> 4;
  if (version !== 1) {
    throw new Error("Unsupported environmental packet version: " + version);
  }

  var flags = bytes[0] & 15;
  var temperature = (bytes[1] << 8) | bytes[2];
  if (temperature & 0x8000) {
    temperature -= 65536;
  }
  var pressurePa = ((bytes[5] << 8) | bytes[6]) * 2;
  // Arithmetic preserves uint32 values above JavaScript's signed bitwise range.
  var gasOhms = bytes[7] * 16777216 + bytes[8] * 65536 + bytes[9] * 256 + bytes[10];

  return {
    format_version: version,
    valid_mask: flags,
    temperature_c: (flags & 1) ? temperature / 100 : null,
    humidity_pct: (flags & 2) ? ((bytes[3] << 8) | bytes[4]) / 100 : null,
    pressure_pa: (flags & 4) ? pressurePa : null,
    pressure_hpa: (flags & 4) ? pressurePa / 100 : null,
    gas_ohms: (flags & 8) ? gasOhms : null
  };
}

// ChirpStack / LoRaWAN Payload Codec API entry point.
function decodeUplink(input) {
  try {
    return { data: Decoder(input.bytes, input.fPort) };
  } catch (error) {
    return { errors: [error.message] };
  }
}
