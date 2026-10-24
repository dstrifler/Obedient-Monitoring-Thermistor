# Six-service firmware implementation

[SOFTWARE_DESIGN.md](SOFTWARE_DESIGN.md) is the functional constitution. This
file maps that design to implementation; [ENVIRONMENT_PACKET.md](ENVIRONMENT_PACKET.md)
is the versioned wire contract.

| Service | Files |
| --- | --- |
| Core/management | `app_service.h/.cpp`, `clock_service.h/.cpp`, Arduino `.ino` wrapper |
| Scheduler | `scheduler_service.h/.cpp` |
| Encoder/decoder | `encoder_service.h/.cpp`, `protocol_types.h`, console JavaScript decoder |
| LoRa | `lorawan_service.h/.cpp`, `radio_hal.h/.cpp`, `lorawan_timer.h/.cpp` |
| Storage | `storage_service.h/.cpp`, private `eeprom_service`, `nonce_store`, `bsec_state` backends |
| Sensor | `sensor_service.h/.cpp`, `bsec_service.h/.cpp`, shared `sensor_types.h/.cpp` |

`config.h` remains unchanged, including provisioning and board/radio constants.
`settings_defaults.h` defines factory preferences; `app_settings.h/.cpp` defines
and validates active application preferences. The former settings/payload
modules, their unused JSON/debug modes, and the unpublished minute-snapshot
reporting draft are replaced by these interfaces.

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
reinitialize BSEC. An acquisition lacking valid T/P/H or gas inputs omits derived
BSEC outputs; normal processing resumes on the next usable conversion. Bus and
algorithm failures still use the bounded recovery path.

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
Factory challenge state is RAM-only and expires one hour from the initial
request. A duplicate does not renew it. Ordinary reports continue; no extra
polling is introduced. Expiry/reboot invalidates the challenge without erasing.

Confirmed reset stops new radio work and requests cooperative worker exit.
Storage is not erased while acquisition might still write. A stop timeout or
three erase-verification failures leaves radio stopped, with an ERROR on Serial
at enabled log levels. Successful verified erase clears all designated stores
and restarts without a persistent reset marker. No automatic recovery from an
interrupted multi-store factory reset is promised.

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
