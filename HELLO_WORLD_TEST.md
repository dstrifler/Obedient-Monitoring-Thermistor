# Temporary Hello World uplink test

This firmware bypasses the BME68x sensor, sensor payload encoders, and saved
application settings. The existing sensor/service files are retained for later
development; the test sketch does not include them or require the BME68x library.
The clock calibration, radio configuration, join procedure, nonce persistence,
and Class A receive windows are unchanged.

After a successful join, the first uplink is sent immediately. Subsequent
successful uplinks are scheduled at 60-second intervals, with normal scheduler
and radio transaction overhead. Failed sends and payload-limit checks retain
the existing five-second retry backoff; failed joins retain the one-minute retry.

- Application FPort: `LORA_UPLINK_FPORT` (currently 1).
- Payload: exactly 11 ASCII bytes, `Hello World`, without a trailing NUL.
- Expected hex: `48656c6c6f20576f726c64`.
- Uplinks remain unconfirmed. A local send-success message does not prove that
  Helium received the packet; verify it in the network console.
- Downlinks are logged in hex. Application settings commands are ignored during
  this test so they cannot change the payload or reporting interval. MAC handling
  remains with RadioLib. Saved application settings are not modified.

## Bench check

1. Build and flash for the actual Apollo3 board using its normal core and RadioLib
   dependencies. This change was checked using a host scheduler simulation; a
   board build was unavailable because no Arduino board platforms or libraries
   were installed in the development environment.
2. Open serial at 115200 baud. Confirm clock initialization and radio startup,
   followed by a successful join. There should be no sensor initialization/read
   attempts. If joining fails, collect the existing timestamp and error logs.
3. In the Helium console, view raw payload bytes and confirm the hex above on
   FPort 1. An existing environmental sensor decoder will need to be bypassed
   when inspecting this temporary text payload.
4. Observe at least three packets approximately one minute apart. Power cycle
   and confirm that the device rejoins and resumes the same text payload.

Sensor bus identification and sensor acquisition are deferred until this test
establishes reliable LoRaWAN communication.
