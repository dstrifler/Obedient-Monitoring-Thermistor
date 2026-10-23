# Environmental packet v2

The firmware sends one **24-byte application packet on FPort 1 every minute**,
starting 60 seconds after OTAA returns. Normal uplinks use **US915 DR1,
SF9/BW125**, with ADR disabled on the node. DR1 is reapplied before capacity
checks/transmission because the stock library can process network rate commands.
Regional join-request rates remain managed by RadioLib. Keep network ADR disabled.

The packet contains temperature, humidity, pressure, raw gas resistance, Bosch
IAQ, CO2-equivalent and breath-VOC-equivalent. These last two are algorithmic
estimates, not independent CO2/VOC measurements. IAQ is not outdoor AQI.

## Wire layout

All multibyte fields are **big-endian**. Invalid measurement slots are zero and
their validity bits are clear; valid zero readings are distinct from unavailable.

| Offset | Bytes | Field | Encoding |
| --- | --- | --- | --- |
| 0 | 1 | Version + environmental validity | High nibble 2; low nibble T/H/P/G |
| 1–2 | 2 | Temperature | Signed int16, degrees C × 100 |
| 3–4 | 2 | Humidity | Unsigned uint16, %RH × 100 |
| 5–6 | 2 | Pressure | Unsigned uint16, Pa / 2, rounded |
| 7–10 | 4 | Raw gas resistance | Unsigned uint32, ohms |
| 11–12 | 2 | Bosch IAQ | Unsigned uint16, index × 10; range 0–500 |
| 13–14 | 2 | CO2-equivalent | Unsigned uint16, whole ppm |
| 15–18 | 4 | Breath-VOC-equivalent | **IEEE-754 binary32**, ppm, unmodified float bits |
| 19 | 1 | BSEC status | Packed fields below |
| 20–23 | 4 | Sample timestamp | Unsigned seconds since boot, not Unix time |

Byte 0 bits 0–3 indicate valid temperature, humidity, pressure and gas. The first
11 slots retain v1's layout, but the version nibble is now 2.

Byte 19:

- Bits 0–1: IAQ accuracy (0 initializing, 1 low, 2 medium, 3 high).
- Bit 2: gas sensor stabilized.
- Bit 3: gas sensor run-in complete.
- Bits 4, 5, 6: IAQ, CO2-equivalent, VOC-equivalent available, respectively.
- Bit 7: temperature/humidity are Bosch sensor-heat-compensated outputs.

Accuracy 0 is **not** the same as missing data. Estimates can be present during
calibration; applications should check accuracy/readiness before treating them
as reliable. The accuracy field refers to IAQ. If Bosch does not provide a valid
compensated T/H pair, raw validated T/H is used and bit 7 is clear. No arbitrary
board temperature offset is applied.

Temperature/humidity retain hundredth-unit encoding, pressure 2-Pa resolution,
IAQ tenths and CO2-equivalent whole ppm. Encoding resolution does not imply
sensor accuracy. VOC retains its original binary32 precision/range. Invalid,
negative or nonfinite estimates are not transmitted as valid measurements.

The timestamp belongs to the acquisition, not the transmit attempt. It advances
through millis rollover in the default BSEC backend. Use network reception time
with uptime/session identity; uptime alone is not a wall clock. There is no
historical buffer/backfill in this update.

## Decoder and MQTT

Paste the **entire** [decoders/environment_v1.js](decoders/environment_v1.js)
into the Helium decoder and save it. Despite its retained filename, it accepts
both 11-byte v1 and 24-byte v2 packets, preserving old decoded fields. It also
supports ChirpStack's decodeUplink(input) API. Empty FPort 0 or 1 packets are
recognized as MAC-only traffic. Malformed input returns an error object rather
than throwing and disabling subsequent Helium decoding.

The v2 fields added to the existing temperature/humidity/pressure/gas object are:

```text
iaq
co2_equivalent_ppm
breath_voc_equivalent_ppm
iaq_accuracy
sensor_stabilized
sensor_run_in_complete
air_quality_valid_mask
temperature_humidity_compensated
sample_uptime_s
```

Unavailable measurements and unavailable IAQ accuracy decode as null. A sample
with valid VOC equal to zero decodes as zero. No Node.js Buffer or DataView is
required by the decoder. MQTT integration topics stay unchanged.

## Scheduling and storage

Bosch controls the LP sensor configuration and heater profile at approximately
three-second intervals. A dedicated Mbed worker services acquisition during
RadioLib's blocking joins and receive windows. A mutex protects whole snapshots;
the main thread performs Serial output and calibration persistence. The worker
does not log, write EEPROM, or operate the radio. Samples older than 10 seconds
are unavailable; delayed conversions beyond the ready grace are discarded.

Calibration is saved after high-accuracy, stabilized/run-in output, then at most
every six hours. It uses a CRC/profile/version-checked record at EEPROM offset
128, outside the settings block. Bulk writes preserve other EEPROM bytes and
readback checks verify the record. Missing/corrupt/incompatible state starts
fresh calibration; save failure retries without stopping radio work. Like the
existing emulated settings EEPROM, this is best-effort single-page persistence:
power loss during a page rewrite can lose its contents. Nonce-journal pages are
independent and are never erased by calibration writes.

Before each radio transaction, capacity is checked at DR1. Its nominal 53-byte
application allowance accommodates the 24-byte report plus up to 15 FOpts bytes.
The existing MAC-only path remains available if capacity is temporarily smaller.
Missing/stale samples produce a 24-byte unavailable packet. Existing radio
retry/recovery handling remains active. Saved field/unit/interval settings and
application downlink commands remain bypassed during testing.

**Adaptive reporting and a six-minute learning period are not implemented.**
Radio reports remain once/minute, including the existing initial 60-second wait.
At 1440 reports/day this is 34560 application bytes, excluding retransmissions,
maintenance exchanges and LoRaWAN/radio framing.

## Build and verification

See [SENSOR_TEST.md](SENSOR_TEST.md) for dependencies, local build instructions,
wiring and physical testing. Host checks:

```sh
python3 tests/check_bsec.py
python3 tests/check_bsec_packet.py
python3 tests/check_decoder.py
python3 tests/check_sensor.py
python3 tests/check_logic.py
python3 tests/check_modules.py
python3 tests/check_logging.py
python3 tests/check_radio_hal.py
python3 tests/check_ide_config.py
```

The BSEC host test simulates the opaque ARM algorithm's public API; actual
algorithm initialization, calibration and concurrent RF/I2C operation require
physical-board testing. The guarded board build links the actual stock binary.
