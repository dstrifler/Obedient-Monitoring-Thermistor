# Temporary Hello World uplink test

The sketch bypasses BME68x acquisition, sensor encoders, and saved application
settings. Sensor service files remain for later development. Application commands
are logged but ignored, keeping the text and successful-report cadence fixed.

- Board: SparkFun LoRa Thing Plus expLoRaBLE (Apollo3).
- Radio oscillator: the NM180100's SX1262 uses a 32 MHz crystal (module
  datasheet section 7.3, page 12). The shared `lwBegin()` path sets RadioLib's
  documented public `XTAL` flag before every radio initialization, including
  recovery. This suppresses the default 1.6 V TCXO control on DIO3. This radio
  oscillator configuration is separate from Apollo3 system-clock calibration.
- DIO1 GPIO handling: `radio_hal.h/.cpp` supplies a firmware-owned
  `Apollo3RadioHal` through RadioLib's public `Module(RadioLibHal*, ...)`
  constructor. Apollo3's `INTDIR_NONE` setting forces GPIO reads to zero, so
  clearing a receive callback can otherwise make a later blocking transmit
  miss DIO1's completion level and return `-5`, despite RF transmission.
  The adapter restores a complete readable input configuration on pad 40 at
  initialization and after interrupt detachment, masks/clears its IRQ while
  detached, and explicitly re-enables the IRQ after registering the next
  receive callback. Only DIO1 gets this handling. SPI1 configuration, timeouts,
  oscillator selection, RF switching and the default 10-ms scan guard retain
  their stock behavior. RadioLib and the Arduino core are unchanged.
  GPIO setup errors latch until radio initialization; join/data wrappers report
  the local error `-31997` and stop transactions instead of repeatedly retrying
  a known GPIO configuration failure. An accepted JoinNonce is still saved if
  a GPIO cleanup failure follows successful activation.
- FPort: `LORA_UPLINK_FPORT` (currently 1).
- Payload: exactly 11 ASCII bytes, `Hello World`, without a trailing NUL.
- Expected hex: `48656c6c6f20576f726c64`.
- First application packet is scheduled no earlier than 60 seconds after
  successful OTAA activation returns, including after recovery and rejoining.
  The scheduler and serial output remain active throughout this wait; no
  blocking one-minute delay is used. Subsequent
  application packets are scheduled 60 seconds apart, subject to radio/network
  eligibility and transaction overhead.
- RadioLib processes JoinAccept parameters before activation returns and MAC
  commands during `sendReceive()`. The Class A radio receives downlinks in the
  transaction's receive windows; idling for a minute does not open extra windows
  or fetch queued commands. MAC commands sent in response to the first uplink
  are therefore processed during that transaction if it reaches reception;
  a transmit error can return before the receive windows and MAC processing.
  Application setting commands remain ignored for this fixed Hello World test.
- Empty application transactions can carry pending MAC commands when the live
  payload limit cannot fit the text. Persistent MAC responses may need further
  downlinks; successful maintenance transactions continue at the normal
  one-minute cadence while the text stays pending. They do not trigger a
  three-attempt fault. Payload eligibility comes from stock RadioLib; see its
  remaining limitations below.
- Uplinks are unconfirmed. Local transaction success does not establish Helium
  receipt. Inspect the network console. INFO reports transaction completion;
  DEBUG adds frame counter, frequency, data rate and receive-window result.
  An absent application downlink correctly reports zero bytes to the application.

## Firmware layout

The Arduino sketch forwards `setup()` and `loop()` to `appSetup()` and
`appLoop()`. Keep the complete project together when building: Arduino compiles
the root `.cpp` files automatically. Each service header declares its public
interface; implementations and mutable module state live in its `.cpp` file.

| Module | Responsibility |
| --- | --- |
| `app_service.h/.cpp` | Application state machine and Hello World cadence |
| `clock_service.h/.cpp` | Crystal calibration and startup timing verification |
| `eeprom_service.h/.cpp` | Settings layout, reserved old nonce area and shared EEPROM bootstrap |
| `lorawan_service.h/.cpp` | Radio, node, credentials, join and transaction wrappers |
| `lorawan_timer.h/.cpp` | Transaction rollover guard |
| `radio_hal.h/.cpp` | Apollo3 DIO1 polling input and receive-interrupt lifecycle |
| `nonce_store.h/.cpp` | Flash journal and checked copies of retained nonce history |
| `log_service.h/.cpp` | Shared serial queue and output service |
| `settings_service.h/.cpp` | Retained settings schema, validation and persistence |
| `payload_service.h/.cpp` | Retained sensor encoders and command decoder |
| `sensor_service.h/.cpp` | Retained acquisition backend and disabled sensor interface |

`config.h`, `log_config.h` and `firmware_features.h` remain configuration
headers. The radio beta defaults to `SENSOR_SERVICE_ENABLED=0` in
`firmware_features.h`; the sensor interface returns false without initializing
a bus or requiring the BME library. Setting it to 1 compiles the retained I2C
backend for future development. The Hello World application continues to bypass
sensor acquisition, saved settings and environmental encoding. Sensor interface
selection and hardware validation remain part of later sensor development.

## Serial output

All firmware-owned output uses `log_service.h` and the shared implementation in
`log_service.cpp`. Output goes only to Serial at 115200 baud. Queued messages and
drop counters are volatile RAM; the logger never writes files, EEPROM or flash.
RadioLib source and its disabled-by-default internal debug output are unchanged.

Set `FW_LOG_LEVEL` in `log_config.h`, or override it at build time with the same
definition for all translation units. The default is `FW_LOG_LEVEL_INFO`.
The supported board script accepts the numeric level, for example
`THERMISTOR_LOG_LEVEL=5 bash tools/check-board.sh` for TRACE; omit the variable
to use `log_config.h`.

| Level | Value | Output |
| --- | --- | --- |
| `FW_LOG_LEVEL_OFF` | 0 | No serial initialization or output |
| `FW_LOG_LEVEL_ERROR` | 1 | Failures that stop operation |
| `FW_LOG_LEVEL_WARN` | 2 | Failed attempts, recovery and degraded operation |
| `FW_LOG_LEVEL_INFO` | 3 | Startup, joins and transaction summaries |
| `FW_LOG_LEVEL_DEBUG` | 4 | Clock/radio details and retry decisions |
| `FW_LOG_LEVEL_TRACE` | 5 | State transitions and application packet hex dumps |

A level includes all more severe messages. Use `LOG_ERROR`, `LOG_WARN`,
`LOG_INFO`, `LOG_DEBUG`, `LOG_TRACE` and `LOG_TRACE_HEX`; disabled macros do not
evaluate their arguments or format messages. Output consists of complete lines
such as `[WARN][LoRaWAN] Join failed: code=-705`. There are no event timestamps.
Preflight output uses the scheduler's existing capacity result; changing the
output level adds no payload-capacity query or PHY configuration.
The old `clockLogTimestamp()` helper and all raw millis/STIMER capture output
have been removed. Clock calibration/rate verification, scheduler deadlines,
rollover protection and fault-message rate limits remain functional timers.

The service owns Serial exclusively and runs on the application's main thread;
do not use it from interrupts or other threads. Its fixed queue holds twelve
records, each at most 159 transmitted bytes including CRLF. Three slots are
reserved against growth by INFO/DEBUG/TRACE messages. Under pressure a more
severe message may evict an unsent, less severe record; partly transmitted lines
are never evicted. If no suitable space exists, the incoming record is dropped,
including ERROR records when the queue is saturated with critical messages.
Saturating counters retain drop totals by level. Once the queue drains, a Logger
message reports the totals (WARN, or ERROR in an ERROR-only build). Long lines
are truncated with `...`; embedded newlines become spaces. Hex dumps use at most
256 bytes of input. Never include credentials in messages at any level.

`logPump()` checks UART readiness before each single-byte write, with limits of
64 bytes and 1 ms per call. The Apollo3 serial backend is configured for
nonblocking writes, and the readiness check also avoids its wrapper's initial
wait. The queue never waits for space. The main loop services output before
state processing, including fault/recovery states; safe startup waits and RTC
measurement polling also service it. No serial draining is added inside
RadioLib's blocking Class A transactions. Logging resumes after those calls
return. `logServiceDelay()` divides startup waits into fixed-count slices so a
faulty millis clock cannot make the logger's startup wait loop unbounded.

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

The EEPROM page at `0xFE000`, including saved application settings, stays
untouched by nonce restoration. The dedicated journal is the only nonce source;
old EEPROM nonce records are neither checked nor migrated, regardless of their
contents. Its old nonce area remains reserved to preserve settings offsets.
An empty journal starts fresh and logs a warning. Before the first join
transmission, the normal reservation path must write and verify a nonce batch
in the journal. Subsequent boots restore that journal and skip unused reserved
values. Existing invalid or damaged journal records still stop joining.

Starting with an empty journal discards any protection provided by old EEPROM
nonce history. With previously used OTAA credentials, this can reuse a DevNonce
and cause network rejection. Skipping EEPROM does not reset network-side history
or reconstruct historical nonces; reconcile network provisioning if this occurs.

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
Three consecutive uplink hardware errors use the same cooldown/recovery path
as join hardware errors. Different hardware error codes count toward the same
streak. A successful transaction or a non-hardware send error clears the uplink
streak; waiting for airtime eligibility does not count as an attempt. A fresh
join or scheduled recovery also clears the streak. This includes SPI errors
before transmission, which otherwise leave normal RX timestamps and would
never trigger the unavailable-transaction workaround.
A receive-side error may occur after actual transmission, so such retries can
produce additional application messages. Lost activation returns to joining.
No-join-accept retries remain one minute apart. Three consecutive join hardware
errors (TX/RX timeout, SPI write/command errors, or chip-not-found) enter the same
cooldown/recovery state as a stuck uplink. Ordinary network join failures clear
that hardware-error streak. Successful joins reset the streak and retry backoff.
Radio initialization gets three attempts. The idle scheduler yields to the RTOS
instead of continuously spinning. It only sleeps for a scheduler deadline within
the normal 50-ms slice. Stale deadlines, including ones that appear positive
after a long execution gap, are resynchronized before radio/fault processing
without a multi-day idle sleep.

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

Radio work pauses during the final ten minutes before the approximately
49.7-day `millis()` rollover. This guard leaves time for the current US915
Class A transaction, including up to 15 NbTrans repetitions, RX2 delays up to
16 seconds, and PHY/CPU margin. The join and send wrappers recheck the guard
immediately before their blocking library calls; activation checks again after
nonce reservation writes. A deferral never authorizes RF or consumes an in-RAM
DevNonce. A reservation completed before deferral remains durable and is skipped
normally during restore.

After the wrap, the scheduler uses the existing 60-second cooldown and recreates
the node, restores nonce history, and joins a fresh OTAA session before sending.
It also detects a wrap missed while the scheduler was suspended, before using
old library timestamps. Permanent fault states remain paused. This intentionally
interrupts the one-minute report cadence around rollover. The guard budget must
be reviewed if the region, modem, or library blocking behavior changes. It is a
budget for normal bounded Class A transactions, not a watchdog for a stuck
driver, prolonged RTOS starvation, or an unbounded chain of automatic MAC-only
responses inside the library.

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
python3 tests/check_logging.py
python3 tests/check_modules.py
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
  Firmware pauses new transactions before rollover and obtains a fresh session
  afterward, as described above. It also handles out-of-range reported waits
  through recovery. Library comparisons remain unchanged.
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
Long-gap regressions cover thirteen idle-deadline boundaries, resuming the
one-minute cadence without catch-up bursts, and keeping faults paused.
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
Additional regressions cover uplink SPI failures before RF, mixed hardware
errors, streak reset rules, cooldown and nonce retention during recovery, join
and send calls within 50 ms of timer rollover, the exact guard boundary, flash
writes entering the guard, missed rollover, and recovery before subsequent RF.
The margin check runs 15 repetitions of the stock transmit method with simulated
one-second uplink airtime, 100-ms PHY setup and 16-second RX2 delay. It checks the
chosen budget for this scenario; it is not a physical worst-case timing proof.
Reclamation tests cover bit-level stale-page erase damage with commit words
intact, repeated interruption/reboot/retry, and every program byte of the
intent/snapshot/activation/new-reservation sequence.
These host fakes do not validate physical flash behavior, crystal accuracy or RF.
The radio startup regression also executes the actual firmware `lwBegin()` and
`lwRecover()` paths with unchanged upstream SX1262/SX126x initialization methods
and their real default arguments. It checks XTAL selection on startup and
recovery, verifies that no TCXO command is requested, and covers radio/RF-switch,
node-initialization and nonce-restore failure propagation. Low-level hardware
operations and the node/flash backend are faked.
Serial checks compile all six levels, verify excluded arguments/strings, shared
state across translation units, queue overflow/priority, retention of partly
transmitted lines, truncation/hex formatting, UART backpressure and write errors,
byte/time budgets across micros rollover, and drop notices. The scheduler check
also holds the UART unwritable through normal reports and a fault, then verifies
that error output drains when the UART becomes ready. A transient SPI-failure
check verifies that verbosity does not change radio operations or error handling.
Set `LOGIC_LOG_LEVEL` to a value from 0 through 5 to check any of the six levels,
including ERROR and WARN without expecting compiled-out INFO congestion. Optional
`LOGGING_SANITIZERS=1` enables address/undefined-behavior instrumentation for the
focused serial checks.
Module checks compile public headers independently and include them from two
consumers. They link the production `.cpp` files separately with fake hardware,
checking shared state, the journal copy API, EEPROM initialization, settings and
payload compatibility, the disabled sensor interface, and Arduino setup/loop
behavior. A separate opt-in sensor build checks the retained backend against a
fake BME library; it does not establish correct wiring or real sensor operation.
Host checks use a staged, non-secret configuration. `MODULE_SANITIZERS=1` adds
address/undefined-behavior instrumentation to the module-link checks.

`python3 tests/check_radio_hal.py` checks the firmware HAL against stock
RadioLib's actual HAL interfaces/implementation and SX126x transmit and callback
methods. It executes the Apollo3 core's GPIO interrupt configuration functions
against simulated registers. A negative control reproduces `-5` after receive
interrupt detachment despite simulated RF delivery; the adapter then completes
100 TX/RX/detach cycles with repeated initialization, verifies working receive
callbacks, detached IRQ masking, unrelated pins, SPI1/timing delegation and
latched GPIO failures. These checks require the installed RadioLib and Apollo3
core used by the board build (`RADIOLIB_UPSTREAM` and `APOLLO3_CORE` can override
their paths). They do not replace a physical-board test: after flashing, verify
multiple consecutive Hello World uplinks with increasing frame counters and
receive-window processing, without the recurring timeout/rejoin cycle.

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
