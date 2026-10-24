# Waveshare BME688 / seven-service firmware bench test

The firmware follows [SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md). BSEC acquires about
every three seconds and commits directly to a one-hour RAM ring. Default RF
reporting is adaptive (1–60 minutes, six-minute baseline). First sensor data
requires six minutes from the first usable reading and all seven measurements;
calibration accuracy zero is permitted. Health packets may precede sensor data.

## Wiring

Keep the existing I2C connection:

| Sensor connector | expLoRaBLE header |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA/MOSI | D16 / SDA1 |
| SCL/SCK | D17 / SCL1 |
| ADDR/MISO | Disconnected |
| CS | Disconnected |

This uses Wire1 and address 0x77, with the Waveshare breakout pull-ups. The Qwiic
Wire connector is not used. Radio pins/SPI1 and XTAL/RF-switch startup are
preserved.

## Stock dependencies

- SparkFun Apollo3 package **2.2.1** (internal platform version 2.0.0).
- RadioLib **7.1.2**.
- Bosch **BME68x Sensor library 1.3.40408**.
- Bosch **bsec2 1.10.2610**, containing BSEC **2.6.1.0**.

No third-party source changes are required. Bosch's stock hard-float Cortex-M4
archive, public headers and BME688 3.3 V / three-second / four-day profile remain
verified. See the [Bosch library](https://github.com/boschsensortec/Bosch-BSEC2-Library)
for its license.

## Update and Arduino IDE setup on Linux

Close Arduino IDE before pulling this change, then reopen the updated `.ino`.
Several old `.cpp`/`.h` modules are deliberately removed; do not restore old IDE
tabs from unsaved buffers. Preserve your local `config.h` provisioning values.

The stock Apollo3 core does not automatically link Bosch's archive. If the
one-time IDE linker configuration is not already installed, run from the repo:

```sh
python3 tools/configure-arduino-ide.py
```

Reopen Arduino IDE, select LoRa Thing Plus expLoRaBLE, and use Verify/Upload.
Arduino CLI is not required. This helper installs a supported board-specific
`boards.local.txt` override, links the stock archive, and protects the nonce
journal flash boundary. Original core/library files remain unchanged. Rerun it
after moving the repository/library or updating the board package.

Discovery overrides: `--platform-dir PATH`, `--sketchbook-dir PATH`,
`--bsec-dir PATH`. To remove only its managed configuration block:

```sh
python3 tools/configure-arduino-ide.py --remove
```

The optional CLI build remains available:

```sh
python3 tools/build-local.py
```

It prints an explicit upload command and does not flash automatically. See
`--help` for CLI/configuration/library/compiler/output overrides. The managed
cloud build uses:

```sh
BSEC_UPSTREAM=/path/to/Bosch-BSEC2-Library bash tools/check-board.sh
```

## First physical test

For the service consolidation, close Arduino IDE before pulling the complete
update, then reopen `Obedient_Monitoring_Thermistor.ino`. Old sub-service files
were removed and must not remain as open/restored sketch tabs. The existing
linker setup still applies if repository/library paths are unchanged. The
consolidation does not change v3 packet bytes or require another decoder update.

1. Save the entire updated [decoder](decoders/environment_v1.js) in Helium before
   expecting new packet fields. The new sensor packet is 25-byte v3; control and
   health messages have an `A1` header. Historical v1/v2 still decode.
2. Upload and open Serial at **115200 baud**. Expect unchanged crystal timing
   verification, BSEC startup, and OTAA join during initialization.
3. Watch the first six minutes of acquisition. No highest-calibration-accuracy
   requirement is imposed, but all seven measurements must be available before
   the first sensor report. Health packets do not open the adaptive gate.
4. Confirm first sensor receipt in Helium/MQTT, then watch adaptive timing.
   Ordinary sensor uplinks remain DR1, SF9/BW125, with network ADR disabled.
5. Confirm factory presentation is Fahrenheit/inHg/ohms/ppb unless valid saved
   preferences were restored. Canonical Celsius/Pa/ohms/ppm fields remain in
   MQTT. Serial reports are approximately once/minute, independently of RF.
6. Exercise `Get Configuration` and `Get Status`, then a harmless unit change
   or fixed-interval batch using [the command builder](ENVIRONMENT_PACKET.md).
   Verify the reply, fresh interval, persistence after reboot, and continued
   acquisition/radio operation. Defaults mode is dynamic; fixed accepts 1–60.
7. Check that a missing field after startup is flagged without losing other
   fields. Sustained losses and recoveries should bring dynamic reporting within
   the six-minute baseline; one transient should not. Fixed mode keeps its
   selected interval. A total sensor failure uses six-minute health in dynamic
   mode or the chosen interval in fixed mode. Restoring the sensor must not force
   a radio rejoin merely because acquisition failed.
8. After BSEC reports high accuracy and calibration has actually been saved,
   reboot and verify calibration restore. LoRa still performs a fresh OTAA join
   with durable nonce history. Ring history and learning restart in RAM.

Do not use emergency reset as a routine settings test: it deliberately erases
calibration and nonce history. Its two-command challenge, expiry, and failure
semantics are documented in [ENVIRONMENT_PACKET.md](ENVIRONMENT_PACKET.md).

BSEC controls its heater/acquisition profile. Temperature/humidity may differ
from the old once/minute raw mode due to sensor-heat compensation. No arbitrary
board-temperature offset is applied. Equivalent gas outputs are estimates and
IAQ confidence must be interpreted alongside accuracy/readiness. No fixed time
to maximum calibration accuracy is promised.

## Host and fallback checks

See [FIRMWARE_ARCHITECTURE.md](FIRMWARE_ARCHITECTURE.md) for the test commands,
service map, adaptive method and persistence layout. Board compilation uses the
actual stock Bosch binary; host BSEC tests simulate its opaque public API.
Host regressions inject invalid T/P/H independently, verify continued partial
publication and recovery without BSEC reinitialization, and cover availability
transitions during blocked main-thread radio work. Settings checks also exercise
every compile-time Serial ceiling, including OFF, with saved TRACE preferences.
Physical calibration, concurrent I2C/RF behavior, and Helium delivery still need
the board tests above.

`THERMISTOR_BSEC_ENABLED=0` with the board helper selects the raw backend;
`THERMISTOR_SENSOR_ENABLED=0` selects radio/health only. The first-complete-report
gate is unchanged: these diagnostics cannot produce all seven BSEC measurements,
so they send health packets and, for raw mode, Serial readings. Compile-time
feature switches remain in `common.h`.
