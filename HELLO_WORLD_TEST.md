# Temporary Hello World uplink test

The sketch bypasses BME68x acquisition, sensor encoders, and saved application
settings. Sensor service files remain for later development. Application commands
are logged but ignored, keeping the text and successful-report cadence fixed.

- Board: SparkFun LoRa Thing Plus expLoRaBLE (Apollo3).
- FPort: `LORA_UPLINK_FPORT` (currently 1).
- Payload: exactly 11 ASCII bytes, `Hello World`, without a trailing NUL.
- Expected hex: `48656c6c6f20576f726c64`.
- First application packet follows successful OTAA activation; subsequent
  application packets are scheduled 60 seconds apart, subject to radio/network
  eligibility and transaction overhead.
- Empty application transactions can carry pending MAC commands when the live
  payload limit cannot fit the text. Three unsuccessful clearance attempts stop
  application work with a diagnostic rather than loop forever.
- Uplinks are unconfirmed. Local transaction success does not establish Helium
  receipt. Inspect the network console. Logs include frame counter, frequency,
  data rate and receive-window result; no downlink correctly reports zero bytes.

## Clock verification

STIMER remains on HFRC/16, matching Arduino/mbed. The 32.768-kHz crystal supplies
RTC time and HFADJ calibration. Startup waits two seconds for the crystal, then
17 seconds after enabling HFADJ (warmup plus four adjustment cycles). It measures
STIMER ticks against the crystal-driven RTC over 20 seconds. This is a coarse
2000-ppm startup sanity check, with RTC endpoint quantization; it is not a proof
of RF receive-window accuracy. Startup takes approximately 41 seconds including
the existing serial delay. Failed configuration, a stopped RTC, or out-of-range
rate pauses radio work with a diagnostic.

## Nonce persistence and recovery

Before an OTAA request can transmit, a verified journal record reserves the next
16 DevNonce values. Failed requests consume in-RAM values; reboot skips to the
stored upper bound. A successful join also saves the latest JoinNonce while
preserving that reservation. The nonce space never wraps. Batching reduces
flash writes; frequent resets deliberately consume unused reserved values.

Two 8-KiB pages at `0xFA000` and `0xFC000` hold append-only 64-byte records with
CRC, sequence number and a commit word written last. Page rollover erases only
the page that does not contain the latest committed record. Flash operation
results and committed records are checked. There is no flash write per uplink.
The legacy EEPROM page at `0xFE000`, including saved application settings, stays
untouched. Valid legacy nonce records migrate with checked reads. Historical
nonces lost by the old firmware cannot be reconstructed.

The supported build script reserves the last 24 KiB of flash with a linker
limit. A runtime image-boundary check also stops writes if the image overlaps
storage. Other build systems must reserve these same pages. Preserve these
pages during flashing: chip erase or overwriting them destroys nonce history.

Invalid credentials/history, corrupt committed records, initialization errors,
and write/readback failures stop joining. The firmware never silently clears
history. Collect the exact error and reconcile the device's network provisioning
before any explicit storage recovery. Resetting the board alone will not repair
invalid records; do not erase storage and reuse the same credentials as a casual
troubleshooting step. Full session restoration and durable ABP frame counters
are not implemented; this test uses OTAA and rejoins after reboot.

## Error handling

Invalid API/configuration errors enter a visible paused state. Airtime deferrals
use RadioLib's time-until-uplink result. Payload/dwell errors retry after a minute;
other transient send failures back off from five seconds to a five-minute cap.
A receive-side error may occur after actual transmission, so such retries can
produce additional application messages. Lost activation returns to joining.
No-join-accept retries remain one minute apart. Radio initialization gets three
attempts. The idle scheduler yields to the RTOS instead of continuously spinning.

## Build and validation

```sh
python3 tests/check_logic.py
bash tools/check-board.sh
```

The board script uses the local environment's Arduino CLI 1.3.1, SparkFun Apollo3
release 2.2.1 and RadioLib 7.1.2. The core archive internally reports 2.0.0. GCC
7-2017q4 is an explicit fallback because SparkFun's prescribed GCC 8 archive
returns HTTP 403. Tests cover journal interruptions/rollover/corruption, durable
nonce reservation before TX, nonce exhaustion, absent/real downlinks, scheduler
cadence/rollover/retries/eligibility/faults, and independent RTC rate checks.
These host fakes do not validate physical flash behavior, crystal accuracy or RF.

## Bench check

1. Build using the guarded script and flash while preserving the storage pages.
2. Open serial at 115200. Allow the clock settling/measurement period. Confirm
   a measured clock result, persistence readiness, radio initialization and join.
   No sensor initialization/read should occur.
3. In Helium, inspect raw bytes on FPort 1. Bypass the old environmental decoder.
   Confirm the text hex above and at least three packets about one minute apart.
4. Verify that successful sends without application downlinks log zero bytes.
5. Reboot after failed joins and confirm that new requests use a higher DevNonce
   using network-side join-request logs. Controlled power-interruption testing
   remains required to validate journal behavior on actual hardware.

Sensor interface identification and acquisition remain deferred.
