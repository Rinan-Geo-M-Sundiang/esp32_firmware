# Guarden ESP32 Firmware — System Documentation

Repository: `Rinan-Geo-M-Sundiang/esp32_firmware`

## 1. Overview

This is Arduino/ESP32 firmware for **Guarden**, a two-compartment greenhouse controller. It has two independent subsystems:

1. **Irrigation** — dual-zone (lowland / highland), threshold + Vapor Pressure Deficit (VPD) gated, with water-volume capping and fault escalation.
2. **Fertilizer dosing** — a single pump that fires on a shared time-of-day/day-of-week schedule pulled from Firebase Realtime Database (RTDB).

The device also reads temperature/humidity/soil-moisture sensors, uploads averages and per-zone readings to Firebase, buffers readings to flash (NVS) when offline, and supports manual dashboard overrides for both irrigation zones and the fertilizer pump.

The codebase is organized so that **decision logic is pure C/C++** (no Arduino dependency, testable on a laptop compiler), while **I/O (Wi-Fi, HTTP, GPIO, NVS)** is isolated behind small, single-purpose modules.

## 2. File Map

| File | Responsibility |
|---|---|
| `sketch.ino` | Entry point: pin/timing definitions, global state, `setup()`, `loop()`, sensor reading, upload orchestration, irrigation call site. |
| `irrigation_types.h` | Data structures and tunable constants for the dual-zone irrigation model. |
| `irrigation_zones.h` | The single authoritative `evaluateZone()` state machine and `logIfChanged()` helper. |
| `water_budget.h` | Per-session water-volume tracking and cap check. |
| `vpd.h` | Vapor Pressure Deficit (Magnus-Tetens) calculation. |
| `config_types.h` | RTDB-fetched config types (`Thresholds`, `FertSchedule`) and pure fertilizer/legacy irrigation decision functions. |
| `pump.h` | Generic `Pump` struct: override-vs-auto arbitration, relay/solenoid actuation, change logging. |
| `rtdb_client.h` | Single HTTP client wrapper for all Firebase RTDB GET/PUT calls. |
| `polled_config.h` | Generic "poll a timestamp, refetch on change" template used for remote config. |
| `json_field.h` | Minimal hand-rolled flat-JSON field extraction (no JSON library). |
| `firebase_uploader.h` | Wi-Fi/NTP bootstrap, JSON body construction, and buffer-vs-push upload decision. |
| `offline_buffer.h` | NVS-backed circular buffer that persists readings across reboots when offline. |

## 3. Hardware / Pin Map

| Signal | Pin | Notes |
|---|---|---|
| DHT22 #1 | GPIO 45 | Lowland sensor A (tomato/eggplant) |
| DHT22 #2 | GPIO 46 | Lowland sensor B (averaged with A) |
| DHT22 #3 | GPIO 47 | Highland sensor (bell pepper) |
| Soil moisture #1 | GPIO 1 | Lowland A |
| Soil moisture #2 | GPIO 2 | Lowland B |
| Soil moisture #3 | GPIO 10 | Highland |
| Water pump relay | GPIO 48 | Shared pump — fires if either zone is open |
| Lowland solenoid | GPIO 16 | |
| Highland solenoid | GPIO 17 | |
| Fertilizer relay | GPIO 18 | |
| Fertilizer solenoid | GPIO 25 | |

All relay/solenoid pins are **active-LOW** (`LOW` = open/on, `HIGH` = closed/off), and are driven `HIGH` (closed) on boot.

**⚠️ Flagged in the source as unverified:** the code comments say the highland pin assignments and the pin map generally should be checked against the actual wiring plan before flashing — they were not confirmed against an external wiring document at the time this code was written.

Soil calibration is linear between two raw ADC endpoints:
- `SOIL_DRY = 0` → 0% moisture
- `SOIL_WET = 5460` → 100% moisture

## 4. Timing Constants

| Constant | Value | Purpose |
|---|---|---|
| `SENSOR_INTERVAL` | 5,000 ms | How often DHT22/soil sensors are read |
| `UPLOAD_INTERVAL` | 10,000 ms | How often averages are pushed to Firebase |
| `CONFIG_POLL_INTERVAL` | 15,000 ms | How often the fertilizer schedule's `*_updated` timestamp is checked |
| `REMOTE_OVERRIDE_CHECK_INTERVAL` | 5,000 ms | How often manual dashboard overrides are polled |

The main `loop()` itself runs roughly every ~100 ms (a `delay(100)` at the end of the loop), with irrigation and fertilizer-pump logic evaluated on every iteration.

## 5. Startup Sequence (`setup()`)

1. Start serial at 115200 baud.
2. Set all relay/solenoid pins to `OUTPUT` and drive them closed (`HIGH`).
3. Initialize the fertilizer `Pump` object (`pumpFertilizer.begin()`), which does the same pin setup for its own pins.
4. Start the three DHT22 sensors and wait ~3.5 s for them to settle.
5. Take an initial sensor reading and build the two zone readings.
6. Start the offline buffer and begin the Wi-Fi connection (`uploaderBegin()`).
7. Wait up to 15 s for Wi-Fi to connect.
8. If connected: start NTP, wait up to 10 s for time sync, and force-fetch the fertilizer schedule from RTDB so the device doesn't run on hardcoded defaults for the first poll interval.
9. If Wi-Fi doesn't connect in time: log a timeout and continue running on the hardcoded default fertilizer schedule (NTP-dependent schedule logic fails closed — see §7).

## 6. Main Loop (`loop()`)

Executed roughly every 100 ms, in this order:

1. **Poll fertilizer schedule** (`fertScheduleCfg.poll()`) — cheap timestamp check every 15 s; only re-fetches the full schedule if the remote `*_updated` value changed.
2. **Read sensors** every 5 s (`readSensors()` + `buildZoneReadings()`).
3. **Poll manual overrides** every 5 s (`checkRemoteOverrideIfNeeded()`) — reads `control/relay_lowland`, `control/relay_highland`, `control/relay_fert` from RTDB.
4. **Drive irrigation** every iteration (`updateIrrigation()`) — see §7.
5. **Drive fertilizer pump** every iteration — evaluates `decideFertilizer()` against the current time and calls `pumpFertilizer.update(...)`.
6. **Upload to Firebase** every 10 s, only if at least one sensor reading is valid this cycle; otherwise the upload is skipped and logged.
7. `delay(100)`.

## 7. Irrigation Subsystem

### 7.1 Data model (`irrigation_types.h`)

- `IrrigationCause`: `NONE | NORMAL | EMERGENCY` — what triggered the current latched irrigation session, if any.
- `ZoneThresholds{ smNormal, smEmergency, smStop }`: soil-moisture (%FC) thresholds. `smEmergency` is always drier than `smNormal` by design.
- `ZoneReading{ moisture, temperature, humidity, valid }`: current sensor snapshot for a zone; `valid = false` forces a fail-closed state.
- `ZoneState{ autoActive, manualOverride, finalActive, sensorFault }`: per-zone runtime state.
- `EmergencyCapHistory{ escalationWindowStartMs, cooldownUntilMs, mechanicalLock }`: tracks repeated emergency-cap events for fault escalation. Escalation memory (6-hour window) and routine cooldown (30-minute re-latch delay) are tracked as **separate fields** so that resetting one doesn't erase the other.

Finalized per-crop thresholds:

| Zone | smNormal | smEmergency | smStop |
|---|---|---|---|
| Lowland (tomato/eggplant) | 70% | 60% | 80% |
| Highland (bell pepper) | 75% | 70% | 80% |

`VPD_START_GATE = 0.6 kPa` gates the *start* of a normal-cause irrigation session only — VPD is never used to stop an already-running session.

**⚠️ `TESTING_FAST_TIMERS` is currently defined** in `irrigation_types.h`. This shrinks all cooldown/escalation/water-cap timers from minutes/hours down to seconds so the full cap → cooldown → re-trigger → escalation → lock cycle can be observed in a few minutes. **This must be removed before flashing to real hardware** — otherwise a real emergency session will cap after ~10 seconds instead of the intended real-world durations, and cooldowns will be seconds instead of 10–30 minutes.

| Timer | Fast-test value | Real value |
|---|---|---|
| Emergency routine cooldown | 20 s | 30 min |
| Emergency escalation window | 60 s | 6 hr |
| Normal cooldown | 10 s | 10 min |

### 7.2 Water budget (`water_budget.h`)

Each zone tracks liters dispensed in the current session (`WaterBudget`). `waterLimitExceeded()` resets the accumulator when a session just opened, and otherwise accumulates `FLOW_RATE_LPM × elapsed_minutes` against a per-zone limit.

**⚠️ Explicitly flagged as unresolved in the source:** `FLOW_RATE_LPM` and both `WATER_LIMIT_*` values are provisional placeholders, not validated against real emitter flow rates. The source comments call for one full manual capped-emergency test per zone before going live. Under `TESTING_FAST_TIMERS`, the lowland limit is set to cap after ~10 s and highland after ~6 s at a 2.0 L/min placeholder flow rate — these are test values only.

### 7.3 VPD calculation (`vpd.h`)

`computeVPD(tempC, relHumidityPct)` uses a standard Magnus-Tetens approximation to compute saturation and actual vapor pressure and returns their difference in kPa.

**⚠️ Explicitly flagged as unresolved in the source:** this implementation has not been checked against reference psychrometric tables; treat its output as unverified pending validation.

### 7.4 Zone state machine (`evaluateZone()`, in `irrigation_zones.h`)

This is the single authoritative decision function for both zones (called once per zone per loop iteration). Evaluation order:

1. **Fail-safe (invalid sensor)** — if `reading.valid` is false, the zone is forced off (`autoActive = false`, `finalActive = false`), `sensorFault = true` is set, and the function returns immediately. This overrides manual override too.
2. **Mechanical lock** — if `capHistory.mechanicalLock` is set (from repeated capped emergencies), the zone is forced off and manual override is **not** consulted. Only an external "dashboard clear-fault" action (outside this function) can reset the lock.
3. **Stop condition** — if soil moisture `sm >= smStop`, the zone's auto-latch is cleared regardless of any other state.
4. **Cooldown-blocked re-latch** — if `sm <= smEmergency` and still within the post-cap cooldown window, a **fresh** latch is blocked (this also implicitly blocks the "normal" branch, since emergency-dry moisture always also satisfies the normal-branch condition). An already-active latched session is left alone.
5. **Emergency** — if `sm <= smEmergency` (and not cooldown-blocked), latch as `EMERGENCY`. This path ignores VPD entirely.
6. **Normal** — if nothing is currently latched, `sm <= smNormal`, VPD is above the start gate, and the normal cooldown has expired, latch as `NORMAL`.
7. Otherwise, whatever is currently latched holds (VPD dropping below the gate mid-session does not stop it — VPD is a start condition only).

After the cause is resolved:

- `finalActive = manualOverride || autoActive`. Manual override intentionally bypasses both the stop condition and cooldowns (treated as a deliberate human action), but **does not** bypass the mechanical lock (step 2) or the fail-closed sensor check (step 1).
- The water budget is checked (`waterLimitExceeded`). If the session is capped:
  - The zone is forced off immediately.
  - If the capped session was `EMERGENCY`: the function checks whether this is the second capped emergency within the 6-hour escalation window. If so, `mechanicalLock` is set (fault escalation). Otherwise this becomes the first tracked event in a new window. A 30-minute (or fast-test 20 s) cooldown is also applied.
  - If the capped session was `NORMAL`: a lighter 10-minute (or fast-test 10 s) cooldown is applied, with no escalation tracking, since normal caps are considered routine.
- The escalation window itself expires (resets to "no pending event") if 6 hours pass with no second capped emergency.

### 7.5 Call site (`updateIrrigation()`, in `sketch.ino`)

Runs every loop iteration:
1. Copies the polled manual-override flags into each `ZoneState`.
2. Computes VPD for each zone from its latest sensor reading.
3. Calls `evaluateZone()` for lowland and highland.
4. Logs fault-lock state transitions.
5. Drives the lowland and highland solenoid pins directly from `finalActive`.
6. Drives the shared water pump relay `ON` if **either** zone is active.
7. Logs on/off state transitions for both zones and the pump.

**⚠️ Flagged as unverified in the source:** the RTDB paths used for the dual-zone manual override (`control/relay_lowland`, `control/relay_highland`) are a design assumption for this revision, not confirmed against an existing dashboard schema.

## 8. Fertilizer Subsystem

### 8.1 Schedule (`config_types.h`)

`FertSchedule{ startHour, startMinute, endHour, endMinute, days[7] }` — `days` is indexed `tm_wday` order (Sunday=0 … Saturday=6). Default schedule: 06:00–06:15, active Mon/Wed/Fri.

`fetchFertSchedule()` fetches `config/fert_schedule` from RTDB and parses it with `json_field.h`, falling back to the hardcoded default on any fetch/parse failure or null payload.

`isFertScheduleActiveNow(schedule, now)` is a pure function (takes `time_t now` as a parameter rather than reading the clock itself, so it's unit-testable off-device):
- Fails closed (`false`) if `now` looks like an un-synced clock (`< 1e9`, i.e. before NTP sync).
- Checks the current weekday against `days[]`.
- Compares current minute-of-day against the start/end window, with support for windows that cross midnight (`start > end`).

`decideFertilizer(schedule, now)` is the function actually used by `loop()` — it simply delegates to `isFertScheduleActiveNow`. There is a single fertilizer pump with no source selection.

### 8.2 Legacy code (kept for reference, unused)

- `decideIrrigation()` in `config_types.h` — the old single-zone threshold irrigation rule, superseded by the dual-zone logic in §7. Not called anywhere in `sketch.ino`.
- `fetchFertActiveSource()` and `decideFertPump()` — from a previous two-source (leachate vs. organic) fertilizer design. Not called; the current firmware uses a single pump with no source arbitration.
- `Thresholds` struct and `fetchRemoteThresholds()` — the legacy single-zone threshold config type; also unused by the current dual-zone code path.

These are intentionally left in the codebase for reference/rollback rather than deleted.

## 9. Pump Abstraction (`pump.h`)

`Pump` is a single, reusable struct that consolidates what used to be duplicated per-pump logic (override arbitration, relay/solenoid actuation, and change-logging):

- `begin()` — sets both pins to `OUTPUT` and closes them (`HIGH`).
- `update(autoDecision)` — computes `desired = overrideActive ? true : autoDecision`, drives both the relay and solenoid pins together, and logs state transitions for both the override flag and the on/off state (distinguishing "override" vs "auto" in the log).

Only the fertilizer pump (`pumpFertilizer`) is currently instantiated as a `Pump` object. The irrigation solenoids/relay in `sketch.ino` are driven directly via `digitalWrite()` rather than through this abstraction.

## 10. Firebase / Networking Layer

### 10.1 `rtdb_client.h`

`RtdbClient` is the single class that talks HTTP to Firebase RTDB. It builds URLs of the form `<db>/<path>.json?auth=<secret>` and exposes:
- `get(path, out)` / `put(path, jsonBody)` — raw GET/PUT, `false` on no-WiFi, connect failure, or bad HTTP status.
- `putFloat(path, value)` — convenience wrapper for writing a single number.
- `getBool(path, default)` — convenience wrapper for reading a boolean control flag.
- `getTimestamp(path)` — convenience wrapper for reading a `*_updated` epoch-seconds field, returning `0` on failure (treated by callers as "no signal yet").

TLS certificate validation is disabled (`client.setInsecure()`) — connections are encrypted but the server certificate is not verified.

### 10.2 `polled_config.h`

`PolledConfig<T>` is a generic template that replaces multiple hand-rolled "poll a timestamp, refetch if changed" implementations with one. It:
- Checks a `*_updated` RTDB timestamp on a fixed interval.
- Only re-invokes the supplied `fetchFn` if the timestamp is new (or on first boot).
- Exposes a `forceFetch()` for use once at startup, and `value()` to read the cached config.

Currently instantiated once, for the fertilizer schedule (`fertScheduleCfg`).

### 10.3 `json_field.h`

A minimal, dependency-free flat-JSON field extractor (`JsonField::rawValue/asFloat/asInt/asBool`) used to parse RTDB responses without pulling in a JSON library. It only handles flat objects (no nesting) and is a shared implementation used by both `fetchRemoteThresholds()` and `fetchFertSchedule()`, replacing what used to be duplicated per-fetcher parsing logic.

### 10.4 `firebase_uploader.h`

Handles:
- **Wi-Fi credentials** and **Firebase DB URL/secret** — currently hardcoded as `#define`s in this file (see §12, Security Notes).
- **NTP sync** (`firebaseInitNTP()`, `waitForNTP()`) — GMT+8 (Philippines) offset, no DST.
- **Epoch helpers** (`firebaseEpochSeconds()`, `firebaseEpochMs()`) — both return `0` if NTP hasn't synced yet, used as a "not ready" sentinel.
- **JSON body construction and push** (`_pushZoneSensor`, `_pushAverages`, `_pushAll`) — writes each valid zone's `sensors/zone-N` and shared `averages/*` paths.
- **`uploadOrBuffer()`** — the main upload decision function called from `loop()`:
  - If Wi-Fi is down, buffers the current average reading to NVS via `OfflineBuffer` and returns.
  - If Wi-Fi is up, first attempts to drain any previously buffered readings (pushing each as an average-only update, stopping the drain on the first REST error and re-queuing that item).
  - Skips the upload entirely if NTP hasn't produced a valid timestamp yet.
  - Pushes the full zone + averages payload; on failure, re-buffers the averages instead of losing the reading.

### 10.5 `offline_buffer.h`

`OfflineBuffer` is an NVS ("Preferences")-backed circular buffer, namespace `"offbuf"`, capacity 50 readings. Each slot is stored as key `r{idx}` → `"temp,hum,ts"` string. Metadata (`count`, `head`, `tail`) is persisted so the buffer survives reboots. When full, the oldest reading is dropped silently on the next push.

## 11. Firebase RTDB Path Schema (as used by this firmware)

| Path | Direction | Purpose |
|---|---|---|
| `config/thresholds` | Read | Legacy single-zone thresholds (unused by current logic) |
| `config/fert_schedule` | Read | Fertilizer schedule object |
| `config/fert_schedule_updated` | Read | Timestamp used to detect schedule changes |
| `config/fert_active_source` | Read | Legacy two-source fertilizer selector (unused) |
| `control/relay_lowland` | Read | Manual override — lowland irrigation zone |
| `control/relay_highland` | Read | Manual override — highland irrigation zone |
| `control/relay_fert` | Read | Manual override — fertilizer pump |
| `sensors/zone-1`, `sensors/zone-2`, `sensors/zone-3` | Write | Per-sensor-set temperature/humidity/moisture + timestamp |
| `averages/temperature`, `averages/humidity`, `averages/moisture` | Write | Rolling averages across all valid sensor sets |

Note: sensor "zones" 1–3 here refer to the three physical sensor sets (two lowland + one highland), which is a different grouping from the two irrigation "zones" (lowland/highland) described in §7.

## 12. Security Notes

- **Hardcoded credentials**: the Wi-Fi SSID and the Firebase RTDB URL + auth secret are stored as plaintext `#define`s directly in `firebase_uploader.h`, which is committed to this public repository. These should be treated as compromised, rotated, and moved out of source control (e.g., into a gitignored config header, NVS, or a secrets-management flow) before any further use of this codebase.
- **TLS verification disabled**: `WiFiClientSecure::setInsecure()` is used for all RTDB calls, meaning the server certificate is not validated. This is common for quick ESP32/Firebase prototypes but leaves the connection open to interception if the network path is untrusted.

## 13. Known Open Items (flagged directly in the source)

These are called out explicitly in code comments as unresolved and should be addressed before production deployment:

1. **Pin mapping** (`sketch.ino`) — verify all pin assignments against the actual wiring plan; not confirmed against an external document.
2. **Manual override RTDB schema** (`sketch.ino`) — the `control/relay_lowland` / `control/relay_highland` paths are an assumption for this revision, not a confirmed dashboard contract.
3. **`FLOW_RATE_LPM` and `WATER_LIMIT_LOWLAND` / `WATER_LIMIT_HIGHLAND`** (`water_budget.h`) — provisional placeholders; the source explicitly calls for one full manual capped-emergency test per zone before going live.
4. **`computeVPD()`** (`vpd.h`) — a working Magnus-Tetens implementation is in place, but has not been checked against reference psychrometric tables.
5. **`TESTING_FAST_TIMERS`** (`irrigation_types.h`) — currently enabled; must be removed/undefined before flashing production hardware, or all cooldown/escalation/water-cap timers will run at test speed (seconds) instead of real-world speed (minutes/hours).
6. **Lowland sensor mapping assumption** (`sketch.ino`) — sensor sets 1 and 2 are assumed to be the lowland compartment (averaged) and set 3 the highland compartment; this mapping is not stated in the original design document and should be verified against actual wiring.

## 14. Build / Test Notes

- Decision logic in `config_types.h` (fertilizer scheduling) is written to be Arduino-independent (`isFertScheduleActiveNow`, `decideFertilizer` take `time_t` directly and use only standard C `time.h`), enabling native/off-device unit testing of the scheduling logic.
- `json_field.h`'s JSON parsing functions are similarly pure (string in, value out) and testable off-device.
- Everything touching `Arduino.h`, `WiFi.h`, `HTTPClient.h`, or `Preferences.h` (sensor reads, RTDB I/O, pin control, NVS buffering) requires the Arduino/ESP32 toolchain and real or emulated hardware.
- Required Arduino libraries: `DHT` (Adafruit or compatible), `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences` — all standard for the ESP32 Arduino core.
