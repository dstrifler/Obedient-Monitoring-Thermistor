# Waveshare BME688 bench test

Sensor diagnostics are enabled by default. The radio sends a fixed 11-byte
environmental packet on FPort 1, starting 60 seconds after OTAA activation
returns and repeating on its existing schedule. Environmental data is also
printed through the shared Serial logger. Saved application settings and
application downlink commands remain bypassed. See
[ENVIRONMENT_PACKET.md](ENVIRONMENT_PACKET.md) for the byte layout.

## Wiring

Disconnect power before wiring the Waveshare BME688 Environmental Sensor:

| Sensor connector label | expLoRaBLE header |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA/MOSI | D16 / SDA1 |
| SCL/SCK | D17 / SCL1 |
| ADDR/MISO | Disconnected |
| CS | Disconnected |

This uses the breakout's documented default I2C interface at `0x77`, with its
onboard address/CS pull-ups. It uses `Wire1`, not the board's Qwiic connector
(`Wire`). The existing non-secret configuration is `SENSOR_USE_WIRE1=1` and
`SENSOR_I2C_ADDR=0x77`. No radio pin or SPI bus is reassigned.

## Build and expected output

Install **BME68x Sensor library by Bosch Sensortec, version 1.3.40408**, through
Arduino Library Manager. Keep stock RadioLib 7.1.2 and the existing SparkFun
Apollo3 board setup and reserved flash layout. No third-party source is patched.
The guarded board script selects the Bosch library at
`$THERMISTOR_TOOLS_DIR/user/libraries/BME68x`; `BME68X_UPSTREAM` can select another
installation directory. Its default tools directory is
`/workspace/.thermistor-tools`.

After startup clock verification, open Serial at **115200 baud**. The monitor
attempts its first measurement when the scheduler starts, then one per minute.
Join/uplink calls can delay collection/printing until RadioLib returns. Example:

```text
[INFO][Sensor] BME68x ready at I2C address 0x77.
[INFO][Sensor] T=25.25 C, RH=50.50 %, P=1001.23 hPa, gas=12345 ohm
```

Gas is reported as `unavailable` unless the sample has both gas-valid and
heater-stable flags and a finite, positive resistance. Temperature, humidity,
and pressure can still be reported in that case. Raw gas resistance is not a
calibrated air-quality score, CO2 concentration, or identification of a gas.
The firmware does not include BSEC processing.

The driver checks initialization, oversampling, heater setup, trigger, and read
status. It schedules the forced-mode wait using Bosch's T/P/H duration plus
the programmed 100-ms heater period and a 5-ms margin. It never deliberately
sleeps through that wait; normal synchronous library/bus operations still run
on the main thread. A missing/not-ready sample gets up to one extra second;
fresh data retained across a blocking radio call is accepted when it returns.
Invalid T/P/H readings are discarded without overwriting the caller's data.

Sensor errors log a warning and retry after 60 seconds; they do not fault or
rejoin the radio. Sampling and radio transactions execute sequentially on the
main thread. There is no sensor access or Serial pumping inside a RadioLib
join/uplink call. Sampling can continue if radio work enters its paused state.
No sensor activity starts if clock/EEPROM startup fails before sensor startup.

To build the radio-only fallback, set `SENSOR_SERVICE_ENABLED=0` in
`firmware_features.h`, or use `THERMISTOR_SENSOR_ENABLED=0` with the board script.
That build does not require the Bosch library or initialize the sensor bus;
it transmits full-length packets with all validity flags cleared.

## Validation

```sh
python3 tests/check_sensor.py
python3 tests/check_logic.py
python3 tests/check_modules.py
python3 tests/check_logging.py
python3 tests/check_radio_hal.py
bash tools/check-board.sh
```

The focused sensor checks compile Bosch's actual unmodified C++ wrapper with
a simulated lower sensor API, then link the real sensor monitor, logger, and
application state machine. They cover forced-mode `getData()` returning zero,
measurement/heater timing, status errors, readiness/timeout/retry, gas flags,
invalid numbers, timer wrap, negative temperature formatting, missing hardware,
UART backpressure, sample freshness, fixed environmental bytes, MAC-only
deferral, and preserved one-minute radio cadence at OFF/INFO/TRACE.
The board build also compiles Bosch's real lower C API. Physical measurements,
sensor disconnection/reconnection, and continued Helium delivery still require
bench verification.

The historical settings-dependent all-fields encoder still occupies 14 bytes.
The active test uses the fixed 11-byte encoder documented in ENVIRONMENT_PACKET.md.
