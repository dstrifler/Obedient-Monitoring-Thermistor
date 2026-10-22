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
  payload limit cannot fit the text. Persistent MAC responses may need further
  downlinks; successful maintenance transactions continue at the normal
  one-minute cadence while the text stays pending. They do not trigger a
  three-attempt fault. Payload eligibility comes from stock RadioLib; see its
  remaining limitations below.
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
16 DevNonce values. The firmware reserves these values before calling the stock
RadioLib activation API. After a successful request transmission, RadioLib
advances the in-RAM nonce; reboot skips unused values to the
stored upper bound. If a failed activation leaves the nonce unchanged, the
firmware uses `setBufferNonces()` to skip to the verified reservation's upper
bound before any retry. A rejected restore disables joining. The next attempt
must reserve a new batch; repeated TX timeouts deliberately consume batches of
16. Successful TX without JoinAccept keeps normal per-request nonce progression.
A successful join also saves the latest JoinNonce while
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
use RadioLib's time-until-uplink result. Valid positive waits are re-queried at
least once a minute, without transmitting early. A reported wait greater than
`0x7FFFFFFF` exceeds the application's supported timer range and enters recovery
instead of becoming a multi-day application sleep. Both the initial eligibility
check and the result after `UPLINK_UNAVAILABLE` use this validation.
Other transient send failures back off from five seconds to a five-minute cap.
Successful activation resets that backoff to five seconds.
A receive-side error may occur after actual transmission, so such retries can
produce additional application messages. Lost activation returns to joining.
No-join-accept retries remain one minute apart. Three consecutive join hardware
errors (TX/RX timeout, SPI write/command errors, or chip-not-found) enter the same
cooldown/recovery state as a stuck uplink. Ordinary network join failures clear
that hardware-error streak. Successful joins reset the streak and retry backoff.
Radio initialization gets three attempts. The idle scheduler yields to the RTOS
instead of continuously spinning.

Three consecutive `UPLINK_UNAVAILABLE` results with zero reported airtime wait
enter an application recovery state. A positive airtime wait clears this count.
The scheduler stops transactions for 60 seconds, allowing abandoned receive
windows to expire, then recreates its node through the public constructor,
initializes the radio, and restores the durable nonce reservation and JoinNonce.
It obtains a fresh OTAA session before sending application data; it never reuses
the discarded session's frame counters. Recovery initialization gets at most
three attempts, a minute apart. Persistence failure stops recovery immediately;
radio initialization failure after the third attempt enters the paused state.
Flash history is retained throughout. Repeated hardware faults can still prevent
rejoining; recovery is not proof of RF connectivity.

After three consecutive payload-size or dwell-time rejections, the firmware
tries a data-rate candidate through public `setDatarate()`. Candidates must have
regional payload capacity for the fixed text plus the maximum 15-byte FOpts
queue; the library verifies the enabled channel plan and radio support. The
regional `payloadLenMax` is the application N-value and already excludes fixed
frame overhead. The full-queue bound is therefore `text bytes + 15`, without an
additional 13 bytes. RadioLib adds and subtracts those 13 bytes internally. A
candidate's reported live capacity must fit the text and stay within its
regional maximum. Each rate is considered at most once until a successful
transaction or new join resets the candidate mask. Retry remains one minute
apart. If no candidate works, the firmware pauses with an explicit capacity
error. MAC queues are retained. Choosing a higher data rate can reduce link
range; normal network ADR remains enabled.

## Build and validation

```sh
python3 tests/check_logic.py
bash tools/check-board.sh
```

The board script selects the installed, unmodified RadioLib 7.1.2 directly.
`tools/verify_radiolib.py` checks its version and the SHA-256 of the two files
previously modified (`RadioLib.h` and `LoRaWAN.cpp`), without writing dependency
files. The patch generator and requirement for a patched-library marker have
been removed. Old generated copies under `patched-libraries` are not selected.
An ordinary IDE build can use stock RadioLib, but must still reserve the flash
layout described above. Set `RADIOLIB_UPSTREAM` for host checks if the library is
outside the default environment.

Dependency policy: keep third-party library source unmodified. Changes belong
in this firmware and its use of the library API. Report upstream limitations
explicitly before choosing a firmware response or a supported upstream upgrade.
Do not restore dependency patches as part of troubleshooting.

### Remaining stock RadioLib 7.1.2 limitations

Stock RadioLib retains the following behavior. Firmware responses are described
where implemented:

- `getMaxPayloadLen()` can wrap to a large unsigned value when pending MAC
  commands exceed the frame capacity. The firmware no longer probes capacity
  by temporarily changing the node's pending-command state. Repeated rejection
  now triggers the bounded public data-rate selection described above.
- Internal absolute timer comparisons are not consistently rollover safe.
  The application's own scheduler uses rollover-safe arithmetic, but that does
  not establish correct library behavior across the roughly 49.7-day wrap.
  Out-of-range reported waits are handled by firmware recovery.
- TX or early RX errors can leave the library's transaction unavailable. The
  firmware detects repeated zero-wait unavailable results and recovers by
  recreating its node and rejoining, as described above.
- A join TX error can return before RadioLib advances DevNonce. The firmware
  detects unchanged nonce state on failed activation and restores the verified
  reservation bound through the public API before permitting another attempt.

The library source remains unchanged. The firmware handles repeated capacity
rejections and invalid reported waits through public APIs. Its recovery policies
do not change the library's internal payload arithmetic or timer comparisons.

The board script uses the local environment's Arduino CLI 1.3.1, SparkFun Apollo3
release 2.2.1 and RadioLib 7.1.2. The core archive internally reports 2.0.0. GCC
7-2017q4 is an explicit fallback because SparkFun's prescribed GCC 8 archive
returns HTTP 403. Tests cover journal interruptions/rollover/corruption, durable
nonce reservation before TX, nonce exhaustion, absent/real downlinks, scheduler
cadence/rollover/retries/eligibility/faults, and independent RTC rate checks.
Checks for the patched library's payload arithmetic, timer comparisons, error
cleanup, and nonce consumption were removed with those patches. Retained checks
exercise firmware scheduling, nonce reservation, the OTAA-only compile guard,
clock sampling/retries, and flash recovery. Integration regressions compile the
unchanged upstream activation, nonce serialization/restore, eligibility, and
transmit methods with the firmware wrappers. They cover TX-timeout nonce skipping,
normal no-JoinAccept progression, restore rejection, exhaustion, failed writes,
node recreation with JoinNonce retention, and scheduler recovery after a timeout.
Scheduler checks also cover eight persistent MAC-only transactions and resuming
text, reset backoff after rejoin, real airtime deferrals, bounded recovery errors,
and cooldown across timer rollover. Additional checks cover periodic long-wait
rechecks without early RF, invalid waits from both eligibility paths, join hardware
error recovery with nonce retention, normal no-JoinAccept handling, and bounded
capacity recovery. Capacity integration tests execute the unchanged upstream
payload, validation and public data-rate setter methods, including restoration
of transmission and exhaustion of permitted candidates. A dependency check accepts the stock
library and rejects edits to the formerly patched files without modifying them.
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
