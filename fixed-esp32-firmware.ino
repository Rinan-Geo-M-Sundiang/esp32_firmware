// main.ino — Guarden ESP32
//
// Irrigation subsystem: dual-zone (lowland/highland), threshold + VPD
// gated, with fault escalation — see irrigation_zones.h /
// irrigation_types.h (finalized v1.0, supersedes the old single-zone
// threshold model that used to live in config_types.h's decideIrrigation()
// — that function is left in place, unused, for reference/rollback only).
//
// Fertilizer subsystem: single pump (one relay, one solenoid), fires
// whenever the shared schedule window is open. No source selection.
#include <DHT.h>

// ─── SIMULATION MODE ────────────────────────────────────────────────────────
// Set to 1 to drive all sensor readings + manual overrides from the Serial
// Monitor instead of real DHT22/soil hardware and Firebase. Sensors don't
// even need to be wired in this mode. Set back to 0 for real deployment —
// nothing else in this file needs to change.
#define SIMULATE_SENSORS 1

#include "rtdb_client.h"
#include "config_types.h"      // FertSchedule / decideFertilizer (fertilizer only)
#include "polled_config.h"
#include "pump.h"               // fertilizer pumps only, as of this revision
#include "firebase_uploader.h"
#include "irrigation_types.h"
#include "water_budget.h"
#include "vpd.h"
#include "irrigation_zones.h"

// ─── Pin definitions ────────────────────────────────────────────────────────
#define DHT_PIN_1   16   // lowland sensor A (tomato/eggplant compartment)
#define DHT_PIN_2   17   // lowland sensor B (averaged with A)
#define DHT_PIN_3   19   // highland sensor (bell pepper compartment)
#define SOIL_PIN_1  35   // lowland sensor A
#define SOIL_PIN_2  32   // lowland sensor B
#define SOIL_PIN_3  34   // highland sensor

// ⚠️ ASSUMPTION — the irrigation doc says pin mapping is "unchanged from
// the original firmware plan," which wasn't included in that doc. Verify
// these three against your actual wiring/pin plan before flashing.
#define RELAY_PIN             26  // shared water pump — fires if either zone is open
#define SOLENOID_PIN_LOWLAND  27
#define SOLENOID_PIN_HIGHLAND 25  // ⚠️ newly assigned, not previously used elsewhere

// Fertilizer pump — single pump, single relay, single solenoid. No source
// selection anymore (previously leachate vs. organic on separate pumps).
#define RELAY_PIN_FERT     33
#define SOLENOID_PIN_FERT  33

// ─── Soil calibration ───────────────────────────────────────────────────────
#define SOIL_DRY 5460  // Raw ADC in dry air  -> 0 % moisture
#define SOIL_WET  0 // Raw ADC in water    -> 100 % moisture

#define DHT_TYPE DHT22
DHT dht1(DHT_PIN_1, DHT_TYPE);
DHT dht2(DHT_PIN_2, DHT_TYPE);
DHT dht3(DHT_PIN_3, DHT_TYPE);

// ─── Timing (ms) ────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL                5000UL
#define UPLOAD_INTERVAL               10000UL
#define CONFIG_POLL_INTERVAL          15000UL
#define REMOTE_OVERRIDE_CHECK_INTERVAL 5000UL

// ─── RTDB client ────────────────────────────────────────────────────────────
RtdbClient rtdb(FIREBASE_DB_URL, FIREBASE_SECRET);

// ─── Fertilizer pump (single pump — one relay, one solenoid) ───────────────
Pump pumpFertilizer("PUMP2(fertilizer)", RELAY_PIN_FERT, SOLENOID_PIN_FERT);

PolledConfig<FertSchedule> fertScheduleCfg(rtdb, "config/fert_schedule_updated",
                                          CONFIG_POLL_INTERVAL, fetchFertSchedule,
                                          FertSchedule{6,0,6,15,{false,true,false,true,false,true,false}});

// ─── Irrigation zone state (dual-zone, finalized logic) ────────────────────
ZoneState lowlandState;
ZoneState highlandState;
ZoneReading lowlandReading  = { 0, 0, 0, false };
ZoneReading highlandReading = { 0, 0, 0, false };

IrrigationCause lowlandCause  = NONE;
IrrigationCause highlandCause = NONE;

EmergencyCapHistory lowlandCapHistory;
EmergencyCapHistory highlandCapHistory;

unsigned long lowlandNormalCooldownUntilMs  = 0;
unsigned long highlandNormalCooldownUntilMs = 0;

WaterBudget lowlandBudget;
WaterBudget highlandBudget;

// ⚠️ ASSUMPTION — RTDB paths for the dual-zone manual override. The doc
// doesn't specify a schema for this revision; adjust to match your actual
// dashboard control paths.
bool remoteOverrideLowland  = false;
bool remoteOverrideHighland = false;

bool lastLoggedLowlandFault  = false;
bool lastLoggedHighlandFault = false;
bool lastLoggedLowland       = false;
bool lastLoggedHighland      = false;
bool lastLoggedPump          = false;

// ─── Runtime sensor state ───────────────────────────────────────────────────
unsigned long lastSensorRead          = 0;
unsigned long lastUpload              = 0;
unsigned long lastRemoteOverrideCheck = 0;

#define NUM_SENSOR_SETS 3
// Defaults chosen so nothing faults/triggers before you send a command:
// 77% moisture sits above BOTH zones' smNormal ceiling (lowland 70,
// highland 75) and below smStop (80) for both — genuinely inert, unlike
// the old 50% default which was actually inside the emergency band
// (<=60 lowland, <=70 highland) and auto-triggered on boot.
float temps[NUM_SENSOR_SETS]       = {25.0f, 25.0f, 25.0f};
float hums[NUM_SENSOR_SETS]        = {60.0f, 60.0f, 60.0f};
float moistures[NUM_SENSOR_SETS]   = {77.0f, 77.0f, 77.0f};
bool  validSensor[NUM_SENSOR_SETS] = {true, true, true};

void buildZoneReadings();  // defined below; used by handleSimSerial()/setup()

// ─── Helpers ────────────────────────────────────────────────────────────────
float soilToPercent(int raw) {
  float pct = 100.0f * float(SOIL_DRY - raw) / float(SOIL_DRY - SOIL_WET);
  return constrain(pct, 0.0f, 100.0f);
}

void readSensors() {
#if SIMULATE_SENSORS
  // Values live in temps[]/hums[]/moistures[]/validSensor[] already —
  // they're written directly by handleSimSerial() from Serial commands.
  // Nothing to do here except echo current state each cycle.
  for (int i = 0; i < NUM_SENSOR_SETS; i++) {
    if (!validSensor[i]) {
      Serial.printf("DHT_ERR sensor-%d (simulated)\n", i + 1);
    } else {
      Serial.printf("SENSOR-%d  temp=%.1f  hum=%.1f  moist=%.1f (simulated)\n",
                    i + 1, temps[i], hums[i], moistures[i]);
    }
  }
#else
  DHT* dhts[NUM_SENSOR_SETS]     = { &dht1, &dht2, &dht3 };
  int  soilPins[NUM_SENSOR_SETS] = { SOIL_PIN_1, SOIL_PIN_2, SOIL_PIN_3 };

  for (int i = 0; i < NUM_SENSOR_SETS; i++) {
    float t = dhts[i]->readTemperature();
    float h = dhts[i]->readHumidity();

    if (isnan(t) || isnan(h)) {
      validSensor[i] = false;
      Serial.printf("DHT_ERR sensor-%d\n", i + 1);
      continue;
    }
    temps[i]       = t;
    hums[i]        = h;
    moistures[i]   = soilToPercent(analogRead(soilPins[i]));
    validSensor[i] = true;
    Serial.printf("SENSOR-%d  temp=%.1f  hum=%.1f  moist=%.1f\n", i + 1, t, h, moistures[i]);
  }
#endif
}

#if SIMULATE_SENSORS
// ─── Serial simulation command parser ──────────────────────────────────────
// Commands (one per line, Serial Monitor line-ending = Newline):
//   L <temp> <hum> <soilRaw>   -> sets lowland sensors 1 & 2 (identical)
//   H <temp> <hum> <soilRaw>   -> sets highland sensor 3
//   FAIL <1|2|3>               -> marks that sensor invalid (disconnect sim)
//   OVR L <0|1>                -> force lowland manual override
//   OVR H <0|1>                -> force highland manual override
//   OVR F <0|1>                -> force fertilizer pump manual override
//   RESET L                    -> clears lowland mechanical lock/cap history
//   RESET H                    -> clears highland mechanical lock/cap history
void applySimReading(int idx, float t, float h, int soilRaw) {
  temps[idx]       = t;
  hums[idx]        = h;
  moistures[idx]   = soilToPercent(soilRaw);
  validSensor[idx] = true;
}

void handleSimSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // Tokenize on spaces (no String.split in Arduino core; do it manually).
  const int MAX_TOK = 5;
  String tok[MAX_TOK];
  int nTok = 0;
  int start = 0;
  for (int i = 0; i <= line.length() && nTok < MAX_TOK; i++) {
    if (i == line.length() || line[i] == ' ') {
      if (i > start) tok[nTok++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (nTok == 0) return;
  tok[0].toUpperCase();

  if ((tok[0] == "L" || tok[0] == "H") && nTok >= 4) {
    float t       = tok[1].toFloat();
    float h       = tok[2].toFloat();
    int   soilRaw = tok[3].toInt();
    if (tok[0] == "L") {
      applySimReading(0, t, h, soilRaw);
      applySimReading(1, t, h, soilRaw);
      Serial.printf("SIM: lowland set  temp=%.1f hum=%.1f soilRaw=%d (moist=%.1f%%)\n",
                    t, h, soilRaw, moistures[0]);
    } else {
      applySimReading(2, t, h, soilRaw);
      Serial.printf("SIM: highland set  temp=%.1f hum=%.1f soilRaw=%d (moist=%.1f%%)\n",
                    t, h, soilRaw, moistures[2]);
    }
    buildZoneReadings();
  } else if (tok[0] == "FAIL" && nTok >= 2) {
    int idx = tok[1].toInt() - 1;
    if (idx >= 0 && idx < NUM_SENSOR_SETS) {
      validSensor[idx] = false;
      Serial.printf("SIM: sensor-%d marked invalid\n", idx + 1);
      buildZoneReadings();
    }
  } else if (tok[0] == "OVR" && nTok >= 3) {
    tok[1].toUpperCase();
    bool val = tok[2].toInt() != 0;
    if (tok[1] == "L") {
      remoteOverrideLowland = val;
      Serial.printf("SIM: lowland override = %s\n", val ? "TRUE" : "FALSE");
    } else if (tok[1] == "H") {
      remoteOverrideHighland = val;
      Serial.printf("SIM: highland override = %s\n", val ? "TRUE" : "FALSE");
    } else if (tok[1] == "F") {
      pumpFertilizer.overrideActive = val;
      Serial.printf("SIM: fertilizer override = %s\n", val ? "TRUE" : "FALSE");
    } else {
      Serial.println("SIM: OVR target must be L, H or F");
    }
  } else if (tok[0] == "RESET" && nTok >= 2) {
    tok[1].toUpperCase();
    if (tok[1] == "L") {
      lowlandCapHistory = EmergencyCapHistory{};
      lowlandNormalCooldownUntilMs = 0;
      Serial.println("SIM: lowland cap history / mechanical lock cleared");
    } else if (tok[1] == "H") {
      highlandCapHistory = EmergencyCapHistory{};
      highlandNormalCooldownUntilMs = 0;
      Serial.println("SIM: highland cap history / mechanical lock cleared");
    }
  } else {
    Serial.println("SIM: unrecognized command. Use: L/H <temp> <hum> <soilRaw> | FAIL <1-3> | OVR L/H/F <0|1> | RESET L/H");
  }
}
#endif

// ⚠️ ASSUMPTION — sensor 1 + sensor 2 = lowland compartment (averaged,
// matches "2 soil + 2 DHT22 averaged" from the design memory), sensor 3 =
// highland compartment (standalone). Verify against your actual wiring —
// this mapping isn't stated in the irrigation doc itself.
void buildZoneReadings() {
  bool lowlandValid = validSensor[0] && validSensor[1];
  if (lowlandValid) {
    lowlandReading.moisture    = (moistures[0] + moistures[1]) / 2.0f;
    lowlandReading.temperature = (temps[0] + temps[1]) / 2.0f;
    lowlandReading.humidity    = (hums[0] + hums[1]) / 2.0f;
  }
  lowlandReading.valid = lowlandValid;

  highlandReading.valid = validSensor[2];
  if (highlandReading.valid) {
    highlandReading.moisture    = moistures[2];
    highlandReading.temperature = temps[2];
    highlandReading.humidity    = hums[2];
  }
}

// Poll remote overrides asynchronously — decoupled from evaluateZone() cadence.
void checkRemoteOverrideIfNeeded() {
#if SIMULATE_SENSORS
  // Overrides are set directly by "OVR L/H/F <0|1>" serial commands instead —
  // no Firebase round-trip in simulation mode.
  return;
#else
  if (millis() - lastRemoteOverrideCheck < REMOTE_OVERRIDE_CHECK_INTERVAL) return;
  lastRemoteOverrideCheck = millis();

  remoteOverrideLowland  = rtdb.getBool("control/relay_lowland");
  remoteOverrideHighland = rtdb.getBool("control/relay_highland");

  pumpFertilizer.overrideActive = rtdb.getBool("control/relay_fert");
#endif
}

// ─── updateIrrigation() — the finalized call site from the design doc ──────
void updateIrrigation() {
  // A cap-cleared override (evaluateZone() forcing manualOverride=false
  // when the water cap hits) must stick. Only pull in a FRESH remote
  // command if the remote flag actually changed since we last saw it —
  // otherwise this line would blindly resurrect the override we just
  // cleared, re-triggering the blip every loop.
  //
  // NOTE: the "last seen" baseline must NOT default to whatever
  // remoteOverride* happens to hold on first call — if a command arrives
  // before updateIrrigation() runs even once, that would be silently
  // treated as "no change" and override would never activate. Use a
  // separate "have we run yet" flag instead of trusting the initial
  // value of the mirrored bool.
  static bool firstRun              = true;
  static bool lastSeenRemoteLowland;
  static bool lastSeenRemoteHighland;

  if (firstRun) {
    lastSeenRemoteLowland  = !remoteOverrideLowland;  // force a mismatch
    lastSeenRemoteHighland = !remoteOverrideHighland; // so tick 0 always syncs
    firstRun = false;
  }

  if (remoteOverrideLowland != lastSeenRemoteLowland) {
    lowlandState.manualOverride = remoteOverrideLowland;
    lastSeenRemoteLowland       = remoteOverrideLowland;
  }
  if (remoteOverrideHighland != lastSeenRemoteHighland) {
    highlandState.manualOverride = remoteOverrideHighland;
    lastSeenRemoteHighland       = remoteOverrideHighland;
  }

  float lowlandVpd  = computeVPD(lowlandReading.temperature,  lowlandReading.humidity);
  float highlandVpd = computeVPD(highlandReading.temperature, highlandReading.humidity);

  evaluateZone(lowlandState,  lowlandReading,  lowlandThresh,  lowlandVpd,
               lowlandCause,  lowlandBudget,  WATER_LIMIT_LOWLAND,
               lowlandCapHistory,  lowlandNormalCooldownUntilMs);
  evaluateZone(highlandState, highlandReading, highlandThresh, highlandVpd,
               highlandCause, highlandBudget, WATER_LIMIT_HIGHLAND,
               highlandCapHistory, highlandNormalCooldownUntilMs);

  // If evaluateZone() just cleared the override (cap hit), sync the
  // remote-mirrored flags too, so a subsequent unrelated remote poll
  // doesn't re-diff against a stale "true" and resurrect it, and so the
  // dashboard's own read-back reflects reality.
  if (!lowlandState.manualOverride && lastSeenRemoteLowland) {
    remoteOverrideLowland     = false;
    lastSeenRemoteLowland     = false;
  }
  if (!highlandState.manualOverride && lastSeenRemoteHighland) {
    remoteOverrideHighland    = false;
    lastSeenRemoteHighland    = false;
  }

  logIfChanged(lowlandCapHistory.mechanicalLock,  lastLoggedLowlandFault,  "LOWLAND_FAULT");
  logIfChanged(highlandCapHistory.mechanicalLock, lastLoggedHighlandFault, "HIGHLAND_FAULT");

  digitalWrite(SOLENOID_PIN_LOWLAND,  lowlandState.finalActive  ? LOW : HIGH);
  digitalWrite(SOLENOID_PIN_HIGHLAND, highlandState.finalActive ? LOW : HIGH);

  bool pumpShouldRun = lowlandState.finalActive || highlandState.finalActive;
  digitalWrite(RELAY_PIN, pumpShouldRun ? LOW : HIGH);

  logIfChanged(lowlandState.finalActive,  lastLoggedLowland,  "LOWLAND");
  logIfChanged(highlandState.finalActive, lastLoggedHighland, "HIGHLAND");
  logIfChanged(pumpShouldRun,             lastLoggedPump,     "PUMP");
}

// ═════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Guarden booting ===");

  pinMode(RELAY_PIN,             OUTPUT);
  pinMode(SOLENOID_PIN_LOWLAND,  OUTPUT);
  pinMode(SOLENOID_PIN_HIGHLAND, OUTPUT);
  digitalWrite(RELAY_PIN,             HIGH); // closed on boot
  digitalWrite(SOLENOID_PIN_LOWLAND,  HIGH);
  digitalWrite(SOLENOID_PIN_HIGHLAND, HIGH);

  pumpFertilizer.begin();

  dht1.begin(); dht2.begin(); dht3.begin();

  Serial.println("Waiting for DHT22 sensors to settle...");
  delay(3500);

  Serial.println("Initial sensor read...");
  readSensors();
  buildZoneReadings();

  uploaderBegin();

  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000UL) {
    delay(500); Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");

    firebaseInitNTP();
    waitForNTP(10000UL);

    fertScheduleCfg.forceFetch();

    Serial.printf("BOOT FERT SCHEDULE  %02d:%02d-%02d:%02d\n",
                  fertScheduleCfg.value().startHour, fertScheduleCfg.value().startMinute,
                  fertScheduleCfg.value().endHour,   fertScheduleCfg.value().endMinute);
  } else {
    Serial.println("WiFi timeout — using default fert schedule, NTP unavailable");
  }

  lastSensorRead          = millis();
  lastUpload              = millis();
  lastRemoteOverrideCheck = millis();
}

// ═════════════════════════════════════════════════════════════════════════
void loop() {
#if SIMULATE_SENSORS
  // 0. Check for a simulation command every loop (~100ms latency, fine for typing).
  handleSimSerial();
#endif

  // 0b. If WiFi failed at boot, request NTP once it's connected — without a
  // valid clock the fertilizer schedule can never fire.
  if (WiFi.status() == WL_CONNECTED) firebaseInitNTP();  // no-op after first call

  // 1. Refresh polled fertilizer schedule (every 15s, only refetches when changed).
  fertScheduleCfg.poll();

  // 2. Read sensors on interval (every 5s).
  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();
    readSensors();
    buildZoneReadings();
  }

  // 3. Poll manual overrides asynchronously (every 5s).
  checkRemoteOverrideIfNeeded();

  // 4. Drive irrigation (dual-zone, finalized logic) every loop, ~100ms.
  updateIrrigation();

  // 5. Drive the fertilizer pump every loop, ~100ms.
  // Interlock: a mechanical lock on either zone means water delivery is
  // suspect, so fertilizer is blocked too — including manual override,
  // mirroring evaluateZone()'s "lock blocks manual" rule.
  bool fertLocked = lowlandCapHistory.mechanicalLock || highlandCapHistory.mechanicalLock;
  static bool lastLoggedFertLock = false;
  logIfChanged(fertLocked, lastLoggedFertLock, "FERT_LOCKOUT");

  time_t now; time(&now);
  bool fertAuto = decideFertilizer(fertScheduleCfg.value(), now) && !fertLocked;
  if (fertLocked) pumpFertilizer.overrideActive = false;
  pumpFertilizer.update(fertAuto);

  // 6. Push to Firebase (every 10s).
  if (millis() - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = millis();

    bool anyValid = validSensor[0] || validSensor[1] || validSensor[2];
    if (!anyValid) {
      Serial.println("UPLOAD_SKIP: no valid sensor readings this cycle");
    } else {
      // Average only over currently-valid sensors — a FAIL'd sensor's last
      // known temps[]/hums[]/moistures[] value is stale and must not be
      // blended into the uploaded average just because the array slot
      // still holds an old number. validSensor[] is the source of truth,
      // not "does the array have something in it."
      float sumTemp = 0, sumHum = 0, sumMoist = 0;
      int   validCount = 0;
      for (int i = 0; i < NUM_SENSOR_SETS; i++) {
        if (!validSensor[i]) continue;
        sumTemp  += temps[i];
        sumHum   += hums[i];
        sumMoist += moistures[i];
        validCount++;
      }
      float avgTemp     = sumTemp  / validCount;
      float avgHum      = sumHum   / validCount;
      float avgMoisture = sumMoist / validCount;

      Serial.printf("AVG  temp=%.1f  hum=%.1f  moisture=%.1f  (from %d/%d valid sensors)\n",
                    avgTemp, avgHum, avgMoisture, validCount, NUM_SENSOR_SETS);

      uploadOrBuffer(rtdb, avgTemp, avgHum, avgMoisture, temps, hums, moistures, validSensor);
    }
  }

  delay(100);
}
