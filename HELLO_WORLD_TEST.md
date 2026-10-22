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
  payload limit cannot fit the text. Three completed clearance transactions
  that still cannot make room stop
  application work with a diagnostic. Radio failures do not consume this budget;
  a new session resets it. If the MAC commands themselves exceed the current
  frame capacity, pause immediately with a data-rate/network diagnostic.
- Uplinks are unconfirmed. Local transaction success does not establish Helium
  receipt. Inspect the network console. Logs include frame counter, frequency,
  data rate and receive-window result; no downlink correctly reports zero bytes.

## Clock verification

STIMER remains on HFRC/16, matching Arduino/mbed. The 32.768-kHz crystal supplies
RTC time and HFADJ calibration. Startup waits two seconds for the crystal, then
17 seconds after enabling HFADJ (warmup plus four adjustment cycles). It measures
STIMER ticks against the crystal-driven RTC over 20 seconds. Each RTC read is
bracketed with STIMER samples; samples taking more than 2 ms are rejected and
retried, with at most five attempts per sample. This handles temporary RTC read
errors and task preemption without accepting a mismatched timestamp pair.
Validation allows three measurement windows, each bounded to 30 seconds. This
is a coarse
2000-ppm startup sanity check, with RTC endpoint quantization; it is not a proof
of RF receive-window accuracy. Startup normally takes approximately 41 seconds
including
the existing serial delay. Retrying failed windows can extend startup to roughly
111 seconds when exhausting the three-window timeout budget. Failed
configuration, a stopped RTC, or out-of-range
rate pauses radio work with a diagnostic.

## Nonce persistence and recovery

Before an OTAA request can transmit, a verified journal record reserves the next
16 DevNonce values. RadioLib consumes the composed DevNonce before starting a
possibly transmitting
operation, so even a TX timeout consumes it; a missing completion IRQ is not
proof that the packet stayed off-air. Failed requests consume in-RAM values;
reboot skips to the
stored upper bound. A successful join also saves the latest JoinNonce while
preserving that reservation. The nonce space never wraps. Batching reduces
flash writes; frequent resets deliberately consume unused reserved values.

Two 8-KiB pages at `0xFA000` and `0xFC000` hold append-only 64-byte records with
CRC, sequence number and a commit word written last. The final two slots on each
page are reserved for CRC-protected erase-intent and activation records.

Before erasing the stale page, the current page durably records its target and
transition sequence. The erased page receives a verified copy of the current
nonce reservation. A separate activation record on the owner page transfers
authority before new reservations can authorize transmission on the target.
After an interrupted erase or snapshot copy, boot retains the owner page,
ignores only the authorized target's debris, and retries reclamation before
saving a new reservation. The newest valid transition determines which page is
current; an old transition cannot authorize ignoring the newer current page.

Recovery retains a complete CRC-valid reservation even if its commit marker is
damaged or unfinished. Ambiguous bodies on the current page, or ambiguous
activation metadata, still stop joining rather than guess or roll back. This
can require explicit recovery after some interrupted metadata/data writes;
automatic recovery is guaranteed here for stale-page erase/copy interruption,
not every flash failure. Flash operations and committed records are checked.
There is no flash write per uplink and no additional flash page is reserved.

Legacy journals with free final slots adopt the new protocol when they roll
over. If an old journal already occupies either reserved transition slot on the
current full page, automatic reclamation stops with an explicit migration error.
It does not erase history to make room. A journal already damaged by an erase
performed without intent metadata also remains ambiguous and requires recovery.

The legacy EEPROM page at `0xFE000`, including saved application settings, stays
untouched. Valid legacy nonce records migrate with checked reads. Historical
nonces lost by the old firmware cannot be reconstructed.

The supported build script reserves the last 24 KiB of flash with a linker
limit. A runtime image-boundary check also stops writes if the image overlaps
storage. Other build systems must reserve these same pages. Preserve these
pages during flashing: chip erase or overwriting them destroys nonce history.

Invalid credentials/history, ambiguous record bodies, initialization errors,
and write/readback failures stop joining. The firmware never silently clears
history. Collect the exact error and reconcile the device's network provisioning
before any explicit storage recovery. Resetting the board alone will not repair
invalid records; do not erase storage and reuse the same credentials as a casual
troubleshooting step. Full session restoration and durable ABP frame counters
are not implemented; ABP is rejected at compile time. This test uses OTAA and
rejoins after reboot.

## Error handling

Invalid API/configuration errors enter a visible paused state. Airtime deferrals
use RadioLib's time-until-uplink result. Payload/dwell errors retry after a minute;
other transient send failures back off from five seconds to a five-minute cap.
On TX or early RX failure, RadioLib waits through the configured RX2 delay plus
a conservative two-second recovery guard, puts the radio in standby, and closes
the abandoned receive-window state before returning the original error. The
next retry can reach the radio instead of looping on UPLINK_UNAVAILABLE.
Duty-cycle timing remains anchored to the attempted transmission.
A receive-side error may occur after actual transmission, so such retries can
produce additional application messages. Lost activation returns to joining.
No-join-accept retries remain one minute apart. Radio initialization gets three
attempts. The idle scheduler yields to the RTOS instead of continuously spinning.

## Build and validation

```sh
python3 tests/check_logic.py
bash tools/check-board.sh
```

The board script prepares an isolated patched copy of RadioLib 7.1.2. It checks
library version and the upstream protocol source SHA-256 before making changes;
the installed upstream library is untouched. The patches saturate payload-limit
subtraction at zero and fix rollover comparisons in eligibility, transmission,
join scheduling and receive-window checks, plus error-path receive-window cleanup
and pre-transmission DevNonce consumption. Protocol-fix marker version 2 is
required so a previously prepared library copy cannot silently omit these fixes. The firmware rejects unpatched builds.
Use this script for board builds; an ordinary IDE build must also use the
prepared library and reserved flash layout. The patch implementation is in
`tools/prepare_radiolib.py`. The host checks need the pristine 7.1.2 library too;
set `RADIOLIB_UPSTREAM` to its directory if it is not in the default environment.

The board script uses the local environment's Arduino CLI 1.3.1, SparkFun Apollo3
release 2.2.1 and RadioLib 7.1.2. The core archive internally reports 2.0.0. GCC
7-2017q4 is an explicit fallback because SparkFun's prescribed GCC 8 archive
returns HTTP 403. Tests cover journal interruptions/rollover/corruption, durable
nonce reservation before TX, nonce exhaustion, absent/real downlinks, scheduler
cadence/rollover/retries/eligibility/faults, and independent RTC rate checks.
Regression checks compile actual patched RadioLib payload and transmission/
eligibility methods, including timer rollover, and test the ABP compile guard.
Additional checks exercise the complete patched sendReceive path after TX/RX
errors, timed-out joins with distinct nonces, successful join derivation using
the transmitted nonce, temporary RTC read errors, delayed samples, and bounded
failure on a persistently invalid or stopped clock.
Reclamation tests cover bit-level stale-page erase damage with commit words
intact, repeated interruption/reboot/retry, and every program byte of the
intent/snapshot/activation/new-reservation sequence.
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
