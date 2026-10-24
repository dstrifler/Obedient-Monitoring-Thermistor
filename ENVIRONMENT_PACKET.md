# Environmental and control protocol

The seven-service firmware sends **25-byte version-3 sensor reports on FPort 1**.
Install the complete [decoder](decoders/environment_v1.js) before flashing this
firmware. Its historical filename is retained; it accepts v1, v2, v3, and the
new control/health packets. Existing MQTT canonical field names remain available.

All multibyte wire fields are **big-endian**. Binary sensor values always use
canonical units. Presentation preferences change decoded/Serial display, never
how an existing canonical field is interpreted. Missing measurements have clear
validity bits; a valid zero remains distinct from unavailable.

## Sensor report v3: 25 bytes

| Offset | Bytes | Field | Encoding |
| --- | --- | --- | --- |
| 0 | 1 | Version and environmental validity | Upper nibble 3; low bits T/H/P/G |
| 1–2 | 2 | Temperature | Signed int16, Celsius × 100 |
| 3–4 | 2 | Humidity | Unsigned uint16, %RH × 100 |
| 5–6 | 2 | Pressure | Unsigned uint16, Pa / 2, rounded |
| 7–10 | 4 | Gas resistance | Unsigned uint32, ohms |
| 11–12 | 2 | Bosch IAQ | Unsigned uint16, index × 10 |
| 13–14 | 2 | CO2-equivalent | Unsigned uint16, whole ppm |
| 15–18 | 4 | Breath-VOC-equivalent | Original IEEE-754 binary32, ppm |
| 19 | 1 | BSEC quality | Packed fields below |
| 20–23 | 4 | Acquisition uptime | Unsigned seconds since boot, not Unix time |
| 24 | 1 | Presentation preferences | Packed fields below |

Byte 19 bits 0–1 contain IAQ accuracy; bit 2 stabilized; bit 3 run-in complete;
bits 4/5/6 identify valid IAQ/CO2eq/VOCeq; bit 7 identifies compensated T/H.
Accuracy zero can be valid initialization output. Estimates are not direct gas
concentration measurements or outdoor AQI.

Byte 24:

- Bit 0: temperature, 0 Celsius / 1 Fahrenheit.
- Bit 1: gas resistance, 0 ohms / 1 kilohms.
- Bit 2: VOC-equivalent, 0 ppm / 1 ppb.
- Bits 3–5: pressure, 0 hPa/mbar, 1 inHg, 2 Pa, 3 kPa, 4 mmHg.
- Bits 6–7: reserved, zero.

Factory preferences encode as `0D`: Fahrenheit, inHg, ohms, ppb. The decoder
retains `temperature_c`, `pressure_pa`, `pressure_hpa`, `gas_ohms`,
`co2_equivalent_ppm`, and `breath_voc_equivalent_ppm`. It adds presentation fields
`temperature`, `pressure`, `gas_resistance`, `breath_voc_equivalent`, each with a
corresponding `_unit` field. Humidity remains `humidity_pct`.

## Control and health uplinks: A1

Control replies use the same FPort 1. They are not sensor reports and do not
satisfy the first-sensor-transmission gate.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | `A1`: application control protocol version 1 |
| 1 | 1 | Kind: 0 result, 1 configuration, 2 status, 3 reset challenge |
| 2–3 | 2 | Request ID, echoed from the downlink |
| 4 | 1 | Command ID, echoed from the downlink; 0 for autonomous health |
| 5 | 1 | Result code |
| 6–9 | 4 | Value: reset challenge for kind 3; otherwise zero |

Results: 0 accepted, 1 invalid, 2 save failed, 3 busy, 4 expired/no pending reset,
5 wrong confirmation code, 6 request-ID conflict.

Result/challenge packets are 10 bytes. Configuration replies are 17 bytes:
bytes 10–16 contain the seven setting values in the table below. They describe
the complete active configuration after a successful batch or defaults reset.
Serial verbosity is the saved preference. A deliberately lower compile-time
`FW_LOG_LEVEL` caps actual output; it does not reject a valid preference or an
unrelated settings batch. The default build includes all six levels.

Status packets are 26 bytes:

| Offset | Bytes | Field |
| --- | --- | --- |
| 10–13 | 4 | Uptime seconds |
| 14–15 | 2 | Current ring record count |
| 16–17 | 2 | Current reporting interval, seconds |
| 18 | 1 | Seven-field validity, bits T/H/P/G/IAQ/CO2eq/VOCeq |
| 19 | 1 | BSEC quality byte, same layout as sensor byte 19 |
| 20 | 1 | Radio state: 0 stopped, 1 initializing, 2 joining, 3 ready, 4 cooldown, 5 rollover wait, 6 fault |
| 21 | 1 | Manager state: low bits 0 starting, 1 running, 2 stopping, 3 erasing, 4 fault; bit 7 reset pending |
| 22–23 | 2 | Signed int16 last radio result |
| 24–25 | 2 | Latest record age in seconds; `FFFF` means no retained record |

Requested status is captured when its command is processed. Automatic health
status is captured when the report is prepared. `packet_type` is `health` for
automatic status (command 0), otherwise `control`. Human-readable result names
and reset confirmation codes are supplied by the decoder.

## Application downlinks: C1

Commands use FPort 1. RadioLib authenticates/decrypts the LoRaWAN message before
application decoding. Do not send the retired two-byte settings commands.

Header: `[C1, command, request_id_hi, request_id_lo]`.

| Command | ID | Additional bytes |
| --- | --- | --- |
| Set settings batch | 1 | Count, then count pairs of setting ID/value |
| Get configuration | 2 | None |
| Get status | 3 | None |
| Reset application settings | 4 | None |
| Request factory reset | 5 | None |
| Confirm factory reset | 6 | Four-byte unsigned confirmation code |

A batch contains 1–7 unique setting IDs. Duplicate/unknown fields, extra bytes,
invalid values, or an empty batch reject the command. The whole batch is
validated, saved, and read back before activation. Save failure keeps the old
active configuration. Accepted mode/interval changes start a fresh interval.

| Setting ID | Name | Values |
| --- | --- | --- |
| 1 | `report_mode` | 0 dynamic, 1 fixed |
| 2 | `interval_minutes` | Every integer 1–60 |
| 3 | `temperature_unit` | 0 C, 1 F |
| 4 | `pressure_unit` | 0 hPa/mbar, 1 inHg, 2 Pa, 3 kPa, 4 mmHg |
| 5 | `gas_unit` | 0 ohms, 1 kilohms |
| 6 | `voc_unit` | 0 ppm, 1 ppb |
| 7 | `serial_level` | 0 OFF, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG, 5 TRACE |

Use a new request ID for each new command, including reset confirmation.
The last eight requests/results are cached in RAM. Repeating the same request
returns the cached result without repeating settings writes or restarting the
interval. Reusing a retained ID for different bytes returns conflict. This is a
bounded retry cache, not durable command history across reboot. Eight replies
can wait in RAM; if that queue is full, new commands have no effects and must be
retried after it drains. Replies are best-effort unconfirmed uplinks.

Replies are queued for the next eligible uplink and can precede a normal sensor
report. A reply does not reset the ordinary sensor deadline. At most one new
application uplink attempt is initiated per minute, and LoRa eligibility still
applies. Complete sensor reports are deferred for insufficient capacity;
MAC-only exchanges can occur, but neither field dropping nor fragmentation is
used. DR1's nominal 53-byte allowance leaves room for these packets and FOpts.

### Generate a console payload

These commands only print payloads; they do not contact Helium or transmit RF:

```sh
python3 tools/downlink.py configuration --id 1
python3 tools/downlink.py set --id 2 --set report_mode=fixed --set interval_minutes=10
python3 tools/downlink.py set --id 3 --set temperature_unit=F --set pressure_unit=inHg
python3 tools/downlink.py status --id 4
```

Copy the printed Base64 into the console downlink field and select Base64,
FPort 1. JavaScript `encodeDownlink(input)` is also included in the decoder for
codec environments that support it.

### Factory reset confirmation

1. Generate a request: `python3 tools/downlink.py reset-request --id 50`.
2. Receive the six-digit code in the challenge uplink/MQTT output.
3. Generate a separate confirmation with the actual received code:
   `python3 tools/downlink.py reset-confirm --id 51 --code 654321`.

The example code is illustrative, not a universal authorization code. Only the
current RAM-held challenge is accepted. It expires one hour after the initial
request, including delivery time; duplicate requests do not extend it. Normal
sensor reporting and Class A windows continue. There is no additional polling,
Class C switch, or persistent reset marker. Wrong, stale, or expired codes do
not erase anything. A reboot discards the challenge.

A valid confirmation stops radio work, cooperatively stops acquisition (up to
10 seconds), erases/verifies EEPROM settings/calibration and both nonce pages,
clears RAM, then resets the MCU. Up to three erase attempts are made. Failure to
stop acquisition prevents erasure; repeated erase failure leaves the radio
stopped and logs an ERROR. Confirmation does not wait indefinitely for another
RF acknowledgement. A power interruption has no reset-resumption guarantee.
Compiled credentials stay unchanged; erased nonce history can require network
or device reprovisioning before further joins are accepted.

## Historical formats

The decoder continues to accept the 11-byte v1 and 24-byte v2 layouts. Sensor
bytes 1–23 of v3 retain v2 meanings; v3 adds preferences and changes the version
nibble. Empty FPort 0/1 frames decode as MAC-only. Malformed input returns a normal
error object, rather than throwing and disabling future Helium decoding.

## Verification

See [SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md),
[FIRMWARE_ARCHITECTURE.md](FIRMWARE_ARCHITECTURE.md), and
[SENSOR_TEST.md](SENSOR_TEST.md). Protocol checks exercise the actual C++ encoder,
all 40 presentation combinations, old decoder compatibility, binary32 edge
values, control/status/challenge packets, and both console command builders.
