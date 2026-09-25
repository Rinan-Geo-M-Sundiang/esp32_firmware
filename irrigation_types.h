// irrigation_types.h — data structures for the dual-zone (lowland/highland)
// threshold + VPD-gated irrigation logic. See the finalized irrigation
// design doc ("e-Tanim — ESP32 Irrigation Logic FINALIZED v1.0") for the
// full grill trail (Q1-Q5) behind these values.
#pragma once
#include <Arduino.h>

enum IrrigationCause { NONE, NORMAL, EMERGENCY };

struct ZoneThresholds {
  float smNormal;     // %FC — normal-start ceiling
  float smEmergency;  // %FC — emergency-start ceiling (drier than smNormal)
  float smStop;       // %FC — stop floor
};

struct ZoneReading {
  float moisture;     // %FC, already averaged/fail-closed upstream
  float temperature;  // deg C
  float humidity;     // % RH
  bool  valid;         // false => fail-closed, evaluateZone() forces OFF
};

struct ZoneState {
  bool autoActive     = false;
  bool manualOverride = false;
  bool finalActive    = false;
  bool sensorFault    = false;
};

// Q2 fix: escalation memory (6-hr window) and routine cooldown (30-min
// re-latch delay) are separate fields — a cooldown reset can no longer
// silently erase the escalation window it's meant to feed.
struct EmergencyCapHistory {
  unsigned long escalationWindowStartMs = 0;  // 0 = no event pending in the 6-hr window
  unsigned long cooldownUntilMs         = 0;  // 0 = not in cooldown
  bool          mechanicalLock          = false; // true = locked, dashboard-clear required
};

#define VPD_START_GATE 0.6f  // kPa — both crops, normal path only

// ── TESTING_FAST_TIMERS: shrinks all cooldown/escalation/cap timers to
// seconds instead of minutes/hours so you can watch a full cap ->
// cooldown -> re-trigger -> escalation -> lock cycle in a few minutes of
// simulator time. DELETE this #define (and the #else branch stays as the
// real values) before flashing to real hardware. ─────────────────────────
#define TESTING_FAST_TIMERS

#ifdef TESTING_FAST_TIMERS
  #define CAPPED_EMERGENCY_ROUTINE_COOLDOWN_MS (20UL * 1000UL)        // 20 sec — time before a zone can re-latch after 1 cap
  #define CAPPED_EMERGENCY_REPEAT_WINDOW_MS    (2UL * 60UL * 1000UL)  // 2 min — escalation memory
  #define CAPPED_NORMAL_COOLDOWN_MS            (15UL * 1000UL)        // 15 sec
#else
  #define CAPPED_EMERGENCY_ROUTINE_COOLDOWN_MS (30UL * 60UL * 1000UL)       // 30 min before re-latch after a 1st cap
  #define CAPPED_EMERGENCY_REPEAT_WINDOW_MS    (6UL  * 60UL * 60UL * 1000UL) // 6 hr — escalation memory span
  #define CAPPED_NORMAL_COOLDOWN_MS            (10UL * 60UL * 1000UL)       // 10 min
#endif

// ── Finalized per-crop thresholds (lowland = tomato + eggplant, shared
// compartment; highland = bell pepper, separate compartment) ───────────────
static ZoneThresholds lowlandThresh  = { 70.0f, 60.0f, 80.0f };
static ZoneThresholds highlandThresh = { 75.0f, 70.0f, 80.0f };