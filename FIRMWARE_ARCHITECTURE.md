# Seven-service firmware implementation

[SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md) is the functional constitution. This
file maps that design to implementation; [ENVIRONMENT_PACKET.md](ENVIRONMENT_PACKET.md)
is the versioned wire contract.

Each service has exactly one public header and one implementation. Supporting
code is private inside that implementation; there are no sub-service files.

| Service | Files | Includes privately |
| --- | --- | --- |
| Core/management | `core_service.h/.cpp` | Crystal-disciplined clock verification and management state machine |
| Sensor | `sensor_service.h/.cpp` | BSEC worker, raw backend, validation, calibration decisions |
| Storage | `storage_service.h/.cpp` | EEPROM bootstrap/layout, calibration record, nonce journal, RAM ring |
| Scheduler | `scheduler_service.h/.cpp` | Reporting policy, learning gates, retry pacing |
| Encoder/decoder | `encoder_service.h/.cpp` | Sensor/control packets and application-command parsing |
| LoRa | `lorawan_service.h/.cpp` | Apollo3 radio HAL, rollover timing, XTAL initialization, joins and recovery |
| Logging | `log_service.h/.cpp` | Cooperative Serial state machine, formatting, queue, overflow and backpressure |

`common.h/.cpp` contains shared settings, sensor/protocol/radio data types,
compile-time sensor features, and pure validation helpers. It has no service
state or hardware/I/O ownership. Header contracts remain acyclic.
`settings_defaults.h` defines factory application preferences; the Arduino `.ino`
is the entry point. `config.h` remains unchanged, including provisioning and
board/radio constants. Those are the only production support files outside the
seven service pairs and common pair; tests, tools and reference sketches remain
separate. Removed filenames are not retained as compatibility headers.

Private implementation sections retain ordinary C++ helpers/classes inside
anonymous namespaces. The host harnesses exercise those sections directly when
hardware needs substituting, without publishing private backend APIs or adding
production test hooks.

## Timing and radio preservation

Core calls the existing crystal-disciplined HFRC verification before storage,
sensor, and radio startup. The clock implementation and radio HAL/timer guards
are preserved. The low-level `lwBegin()` sequence retains the discovered XTAL
selection (zero TCXO voltage), DIO2 RF switching, and ADR/DR1 behavior. Recovery
reconstructs the node and repeats the same initialization with nonce history.
No core or RadioLib source patches are required.

LoRa owns its initialization/join/recovery states. Missing JoinAccept retries
are separated from repeated hardware failures. Three hardware failures initiate
a one-minute recovery cooldown; recovery attempts are bounded. Permanent
configuration/persistence errors halt radio work. Rollover pauses and fresh
recovery prevent reusing old absolute RadioLib timestamps across wrap.

Scheduler initiates encoding; encoder reads a coherent latest record and active
settings, then invokes LoRa. Scheduler records the outcome. Downlinks return to
management only after the Class A transaction completes. Control replies do not
reset the ordinary sensor-report deadline or count as the first sensor report.
Regular health packets occupy fixed-mode reporting slots, preserving the chosen
interval through sensor loss and recovery.

## Cooperative logging

Core initializes logging and calls `logPump()` from its main loop and safe
startup waits. No logging thread is created. The service owns the Serial sink,
message formatting, twelve fixed queue slots, severity-aware eviction and
cumulative drop accounting. Each pump emits at most 64 bytes or one millisecond
of work; a nonwritable Serial port never makes acquisition wait.

`logState()` exposes `Stopped`, `Disabled`, `Idle`, `Queued`, or
`Backpressured`. Queueing, bounded pumping, runtime OFF/re-enable, and Serial
readiness drive transitions. A compile-time OFF build reports `Disabled` without
initializing Serial. Runtime OFF discards pending output. Worker threads and ISRs
do not call the logger; sensor diagnostics/persistence are polled on main.
Radio code emits only outside its precise receive-window work.

## RAM history and concurrency

BSEC's normal-priority worker owns I2C and BSEC calls. Each completed acquisition
commits a record directly into storage, approximately every three seconds. A
mutex protects the ring; a lightweight atomic notification wakes scheduler
policy after the complete record is visible. Serial and EEPROM operations
remain on main. No worker holds the ring mutex during Serial, flash, or RF work.

The ring has 1,200 records, at most 57,600 bytes. Timestamp pruning excludes
records older than one hour, including during a sensor outage. Generation
checks reject an analysis pass if a concurrent commit changed its view. A
complete copy of the history is not maintained elsewhere.

Records older than 10 seconds are unavailable for new transmissions. Missing
acquisitions are not fabricated. The first usable reading starts a six-minute
elapsed-time gate; first sensor TX also requires all seven fields, with accuracy
zero permitted. Startup health and subsequent total-failure health use six
minutes in dynamic mode or the selected fixed interval. BSEC acquisition is
independent of these report deadlines.

Validate temperature, humidity, pressure, and gas independently. A numeric
failure leaves the other raw fields available and does not pause acquisition or
reinitialize BSEC. Subscribe to `BSEC_OUTPUT_RAW_PRESSURE` alongside the seven
derived/status outputs so BSEC enables pressure acquisition through its public
API. A skipped pressure channel is unavailable even if its compensated number
is within range; store NaN so the shared validity mask, Serial formatter, and
packet encoder cannot present stale pressure as a fresh measurement.

An acquisition lacking valid temperature/humidity, gas, or pressure required by
BSEC's current process mask omits derived BSEC outputs. If BSEC does not request
pressure, the remaining valid inputs may still produce air-quality outputs.
Normal processing resumes on the next usable conversion. Bus and algorithm
failures still use the bounded recovery path. Stored pressure and BSEC C inputs
use Pa; packet scaling and decoder display conversions remain independent.

## Dynamic reporting method

The scheduler examines ring data on commit notifications in dynamic mode.
Fixed mode uses timers and does not run per-record adaptive analysis. Both modes
retain timers for delivery and status when no acquisitions arrive.

Analysis uses reusable statistical scratch memory, not another sensor history:

1. Group the current hour into 120 thirty-second bins and compute per-metric
   medians. A bin needs at least three valid acquisitions for that metric.
   Individual unexpected samples therefore have limited influence.
2. Transform gas resistance and equivalent-gas values with `log1p` for relative
   comparisons. Environmental values and IAQ retain their canonical scales.
3. Derive a robust baseline slope from older adjacent-bin differences. Estimate
   normal noise using the median absolute deviation of those differences.
   Encoding/numerical resolution supplies a lower bound, not a user threshold.
4. Compare the latest available 90 seconds with that baseline. Recent movement
   and persistent departures from the local trend contribute activity. Metrics
   are evaluated independently; missing fields do not dilute an active field.
5. Convert activity to an interval around the six-minute baseline, clamped to
   60–3,600 seconds. Insufficient history uses six minutes.
6. Smooth interval decreases with a one-minute half-life; allow increases at at
   most one second per elapsed second. These rates depend on elapsed time, not
   how many commit notifications arrive. A detected sustained availability loss
   or recovery keeps a report pending within the baseline until a sensor report
   is sent.

Readiness changes are debounced independently of value statistics. Inspect the
newest consecutive acquisitions in the ring: at least three matching readiness
observations spanning six seconds must occur within thirty seconds, without
crossing a stale acquisition gap. Compare qualified fields with the previously
evaluated readiness mask, starting with all seven at the first complete report.
That mask is scheduling state, not cached measurement values. A single transient
does not change it. This handles both returning fields and commit notifications
coalesced during blocking RF work; no particular old bin must still contain the
transition. Fixed mode does not run this analysis or accelerate its schedule.

The scheduler learns before first TX but does not change delivery timing until
a complete first sensor packet succeeds locally. There is no separate last-sent
measurement baseline. Transmission timestamps and pending deadlines are normal
scheduler control state. At least 60 seconds separates new application uplink
attempts. Retries back off to at most five minutes; RadioLib may require longer
eligibility waits, which are never overridden.

## Persistence and settings

Application settings use an explicit 32-byte version-3 record at EEPROM offset
32, with CRC/readback verification. Valid legacy v2 settings are read-only
migrated: preserve their unit choices and fixed-interval preset, supply newly
introduced fields from defaults. Old field-selection/payload-mode flags do not
remove measurements from the new report. Unsupported/corrupt settings fall back
to defaults without erasing other stores.

BSEC calibration retains its separate record at offset 128. Sensor supplies the
profile compatibility tag and owns calibration decisions; storage performs I/O
and integrity checks. Calibration writes preserve settings. Both use the
existing single-page emulated EEPROM and do not provide power-loss-atomic
rollback. OTA batches preserve old active RAM settings on save failure; a later
boot still validates whatever durable record remains.

Nonce history remains in its two independent flash pages and retains its
pre-transmit high-water reservation and interrupted-reclamation checks. Ordinary
settings/calibration operations and recovery never erase those pages. Explicit,
confirmed factory reset is the only application path to complete erasure.

## Management, command replies, and reset

Core validates whole command batches, saves/verifies before activation, and
notifies scheduler of accepted timing changes. Those changes start a fresh
interval; they do not restart acquisition, clear history, or bypass startup
requirements. Runtime Serial verbosity supports all six levels in the default
build; `FW_LOG_LEVEL` can deliberately set a lower compile-time ceiling. Startup
timing diagnostics use INFO until persisted application settings are restored.
The saved verbosity remains a preference in the range OFF–TRACE. The build
ceiling caps actual output without invalidating that preference, other OTA
changes, or factory application defaults. Configuration replies report the saved
preference; `logGetLevel()` returns the effective capped level.

Reply and retry caches are bounded at eight entries each. Cached request IDs
prevent repeated settings writes or deadline restarts within that window.
Factory-reset and reboot challenge state is RAM-only and expires two hours from
the initial request. A duplicate does not renew it. Only one operation may be
pending; confirmation must match both its type and six-digit code. Ordinary
reports continue; no extra polling is introduced. Expiry/reboot invalidates the
challenge without erasing, including any challenge still waiting in the reply
queue. Reply kinds 3/4 distinguish factory-reset/reboot challenges; status bits
7/6 respectively identify their pending state.

Confirmed reset stops new radio work and requests cooperative worker exit.
Storage is not erased while acquisition might still write. A stop timeout or
three erase-verification failures leaves radio stopped, with an ERROR on Serial
at enabled log levels. Successful verified erase clears all designated stores
and restarts without a persistent reset marker. No automatic recovery from an
interrupted multi-store factory reset is promised.

Confirmed reboot uses the same cooperative stop and ten-second stop timeout,
then resets the MCU directly, bypassing the erase state entirely. Settings,
calibration and nonce history remain byte-for-byte intact; normal startup
restores them while RAM history/learning/session state starts fresh. Neither
confirmed operation waits for another RF acknowledgement before restart.

## Checks

```sh
python3 tests/check_system.py
python3 tests/check_logic.py
python3 tests/check_radio_lifecycle.py
python3 tests/check_bsec.py
python3 tests/check_sensor.py
python3 tests/check_modules.py
python3 tests/check_logging.py
python3 tests/check_radio_hal.py
python3 tests/check_decoder.py
python3 tests/check_bsec_packet.py
python3 tests/check_protocol.py
python3 tests/check_ide_config.py
```

`HOST_SANITIZERS=1 python3 tests/check_system.py` adds undefined-behavior checks.
Host suites use real firmware and public dependency code with hardware/opaque
algorithm substitutes. They do not establish physical calibration quality or
network receipt; the board/Helium bench checks remain necessary.
