# Waveshare BME688 / Bosch BSEC bench test

The default backend uses Bosch BSEC 2.6.1.0 for IAQ, CO2-equivalent and
breath-VOC-equivalent, plus sensor-heat-compensated temperature/humidity where
available, pressure and raw gas resistance. Radio reports are 24 bytes every
minute, first sent 60 seconds after activation. See
[ENVIRONMENT_PACKET.md](ENVIRONMENT_PACKET.md) for the wire contract/decoder.

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

This uses Wire1 and address 0x77, with the Waveshare breakout's default pull-ups.
The Qwiic Wire connector is not used. Radio pins/SPI1 are unchanged.

## Stock dependencies

- SparkFun Apollo3 board package 2.2.1 (internal platform 2.0.0).
- RadioLib **7.1.2**.
- Bosch **BME68x Sensor library 1.3.40408**.
- Bosch **bsec2 1.10.2610**, containing BSEC **2.6.1.0**.

Install the libraries through Arduino Library Manager or Arduino CLI. The build
verifier checks Bosch's hard-float Cortex-M4 archive, public headers and
bme688_sel_33v_3s_4d profile against the supported upstream release. Dependencies
are used without source changes. [Bosch upstream](https://github.com/boschsensortec/Bosch-BSEC2-Library)
provides the library and license.

## Arduino IDE setup on Linux

The stock Apollo3 core's linker recipe does not automatically link BSEC's
precompiled archive. After installing the dependencies above, close Arduino IDE
and run this once from the repository directory (Python 3 required):

```sh
python3 tools/configure-arduino-ide.py
```

Reopen Arduino IDE, select LoRa Thing Plus expLoRaBLE, and use Verify and Upload
normally. Arduino CLI is not required for this setup.

The helper creates a supported `boards.local.txt` override next to the installed
Apollo3 `boards.txt`. Its settings apply only to expLoRaBLE: link Bosch's
hard-float Cortex-M4 archive, enforce the nonce-journal flash boundary with
`tools/nonce_guard.ld`, and limit the IDE's displayed upload capacity. The
original core and library files stay unchanged. Existing local settings are
preserved and backed up before the first change.

The helper reads the IDE's sketchbook path from `~/.arduino15/preferences.txt`
and discovers the installed Apollo3 package. If discovery is ambiguous, use
`--platform-dir PATH` for the folder containing `platform.txt`/`boards.txt`,
`--sketchbook-dir PATH` for your sketchbook, or `--bsec-dir PATH` for the Bosch
library folder containing `library.properties`.

Rerun after moving the repository or Bosch library, or updating the board
package: the override contains absolute paths. Keep the repository available
while building. To remove only these overrides, close the IDE and run:

```sh
python3 tools/configure-arduino-ide.py --remove
```

## Optional Arduino CLI build on Linux

For a per-build alternative that does not install local board overrides,
install [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/)
and run:

```sh
python3 tools/build-local.py
```

If needed, --config-file PATH selects an Arduino CLI configuration that uses
your installed Arduino sketchbook/packages. --bsec-dir PATH selects an
unmodified BSEC library outside the usual sketchbook. --cli PATH and
--compiler-path PATH select existing tools; they do not install/change tools.

The helper stages an IDE-compatible sketch name, checks dependencies, generates
a private linker script, builds, and places firmware files in the sibling
<repository-name>_build directory. It prints an explicit upload command:

```sh
arduino-cli upload --fqbn SparkFun:apollo3:lora_thing_plus --port /dev/ttyUSB0 --input-dir "../Obedient_Monitoring_Thermistor_build"
```

Use the actual output path/device port. Building does not upload automatically.
Both build and upload use the same CLI configuration when one is required.

In the managed build environment:

```sh
BSEC_UPSTREAM=/path/to/Bosch-BSEC2-Library bash tools/check-board.sh
```

Its default BSEC installation is
$THERMISTOR_TOOLS_DIR/user/libraries/bsec2. BME68X_UPSTREAM can similarly select
the stock sensor library. No third-party code or board-core files are modified
by either build helper.

## First physical test

1. Update/save the complete decoder before expecting the new fields in MQTT.
2. Flash and open Serial at 115200. Expect BSEC startup, normal OTAA join, and
   the first 24-byte FPort 1 report 60 seconds after joining.
3. Confirm normal uplinks show SF9/BW125 (DR1). Leave network ADR disabled.
4. Check all environmental values and the three estimates in decoded MQTT data.
   VOC is an estimate in **ppm**; Serial displays rounded **ppb**.
5. Watch IAQ accuracy/readiness. Accuracy 0 during initial calibration is
   expected; it does not establish reliable air quality. There is no promised
   fixed time to reach accuracy 3.
6. Confirm sampling continues during joins/receive windows, stable reporting,
   and no new Mbed fault. This concurrency needs physical validation.
7. After calibration has been saved, reboot and check continued calibration
   behavior. LoRaWAN still performs a fresh OTAA join with preserved nonces.

Temperature/humidity may differ from the former once/minute raw readings because
sampling/heater cadence changes and Bosch compensates sensor self-heating. No
board-specific offset is guessed. Move away from vents for room measurements.
CO2-equivalent/VOC-equivalent are algorithm estimates, not independent gas
concentrations or a substitute for a CO/exhaust detector.

Sensor errors retry after 60 seconds, clear stale data, and do not force a radio
rejoin. BSEC runs only on a normal worker thread; Serial and EEPROM stay on the
main thread. EEPROM calibration storage and nonce journal are independent.

## Fallback builds

THERMISTOR_BSEC_ENABLED=0 selects the previous raw sensor backend with the
guarded board script. It sends v2 environmental slots, with BSEC estimates
unavailable. It samples once/minute and has no BSEC dependency. Setting
THERMISTOR_SENSOR_ENABLED=0 builds without sensor dependencies/bus activity;
reports retain their 24-byte format with all measurements unavailable.

Equivalent compile-time switches are in firmware_features.h. These fallbacks
do not add adaptive reporting. Host raw-backend tests continue using the actual
unmodified Bosch wrapper; BSEC tests use the real public headers with hardware
and opaque algorithm calls simulated. Board builds link the actual ARM binary.
