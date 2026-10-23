# Environmental packet v1

The current test sends a fixed **11-byte binary application payload on FPort 1**.
It replaces Hello World. It starts 60 seconds after a successful OTAA activation
returns, including after radio recovery, then uses the existing one-minute
cadence and airtime/retry handling. Saved report interval, field selection,
units, payload-mode settings, and application downlink commands remain bypassed
while testing. The Helium decoder will be implemented separately from this
board-side change.

## Byte layout

All multibyte integers are **big-endian**. Byte positions never change when a
measurement is invalid; its slot contains zero and its validity bit is cleared.
Zero alone must not be interpreted as missing: 0 C and 0 %RH are valid readings.

| Byte offset | Field | Encoding | Decoder conversion |
| --- | --- | --- | --- |
| 0 | Version and validity | High nibble = 1; low nibble = valid fields | `version = byte0 >> 4` |
| 1-2 | Temperature | Signed int16, degrees C multiplied by 100 | Divide signed value by 100 |
| 3-4 | Humidity | Unsigned uint16, %RH multiplied by 100 | Divide by 100 |
| 5-6 | Pressure | Unsigned uint16, Pa divided by 2, rounded | Multiply by 2 for Pa; divide by 50 for hPa |
| 7-10 | Gas resistance | Unsigned uint32, ohms | Read as an unsigned 32-bit value |

Validity bits in byte 0:

- Bit 0 (`0x01`): temperature valid.
- Bit 1 (`0x02`): humidity valid.
- Bit 2 (`0x04`): pressure valid.
- Bit 3 (`0x08`): gas resistance valid.

Thus `0x1F` means all four values are valid; `0x17` means temperature, humidity,
and pressure are valid with gas unavailable; `0x10` means no fresh valid sample.
Pressure has 2-Pa resolution, a rounding error of at most 1 Pa, and covers the
BME688's full 30000-110000 Pa measurement range. Temperature and humidity are
rounded to hundredths. Encoding precision does not imply sensor accuracy.

Gas resistance is raw ohms, not an IAQ score or gas concentration. Its bit is
set only when acquisition reports both gas-valid and heater-stable flags and
the resistance is usable. Preserve unsigned interpretation in JavaScript for
values above `0x7FFFFFFF`.

## Known encoding examples

```text
Input: 25.25 C, 50.50 %RH, 100123 Pa, 12345 ohm; all valid
Hex:   1F 09 DD 13 BA C3 8E 00 00 30 39
Decode:25.25 C, 50.50 %RH, 100124 Pa, 12345 ohm

Same T/H/P with gas unavailable:
Hex:   17 09 DD 13 BA C3 8E 00 00 00 00

Sensor absent, failed, or sample stale:
Hex:   10 00 00 00 00 00 00 00 00 00 00
```

A decoder should accept FPort 1, require exactly 11 bytes and version 1, decode
temperature as signed two's complement, and show invalid fields as unavailable.
These bytes are incompatible with the historical 14-byte settings-dependent
binary encoder and with Hello World's ASCII bytes. No new checksum, timestamp,
or sequence counter is included; LoRaWAN and the network supply integrity,
frame counters, and reception timestamps.

## Acquisition and radio behavior

The monitor samples independently about once a minute and retains the latest
validated sample in RAM. Each RF attempt encodes one snapshot. Samples older
than **65 seconds** are unavailable, providing five seconds of tolerance beyond
the measurement interval. Age is measured from estimated conversion completion,
including when collection is delayed by a blocking radio call. Acquisition
failure invalidates the retained sample. An absent/stale sample produces the
full-length unavailable packet instead of replaying an old measurement.

The packet size is always 11 bytes at every data rate. Before each transaction,
firmware checks RadioLib's current available payload capacity. If pending MAC
commands leave less than 11 bytes, it runs the existing MAC-only transaction
and retries environmental reporting on the normal cadence. The next report
uses the latest fresh sample; missed samples are not buffered as a backlog.
No sensor field is removed to fit a smaller packet. RF failures retain the
existing retry/recovery policy and can change the actual reporting interval.

## Nominal daily data

With uninterrupted one-minute sensor reporting, excluding startup, deferrals,
MAC-only exchanges, and retransmissions:

- 1440 environmental packets per 24 hours.
- 15840 application bytes: 15.84 kB, or 15.47 KiB.
- At least 34560 LoRaWAN frame bytes with minimum 13-byte framing per packet,
  before additional MAC commands and LoRa radio framing/preamble overhead.

## Bench verification

Keep the wiring and stock dependencies in [SENSOR_TEST.md](SENSOR_TEST.md).
After merging/pulling and flashing, inspect raw FPort 1 payloads in Helium:
expect 11 bytes with a leading `1F`, `17`, or another valid version-1 flag
combination, rather than ASCII Hello World. Check changing readings against
Serial at 115200, one-minute cadence, and increasing network frame counters.
Sensor disconnection/reconnection should clear/recover validity without
causing a new join. Physical RF receipt still needs the board/network test.
