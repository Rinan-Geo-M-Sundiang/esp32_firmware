// water_budget.h — per-session water volume tracking + cap check.
//
// ⚠️ STILL OPEN per the finalized irrigation doc, §5 "Still open — not
// resolved by this document": FLOW_RATE_LPM and the two WATER_LIMIT_*
// values are listed as a HARD PRE-DEPLOYMENT BLOCKER — the doc calls for
// "one full manual capped-emergency test per zone before going live."
// The numbers below are provisional placeholders only, so the firmware
// compiles and runs — DO NOT treat them as validated.
#pragma once
#include <Arduino.h>

// TESTING_FAST_TIMERS (see irrigation_types.h) also shrinks the water cap
// so a session caps in ~10 seconds instead of ~90, so you can actually
// watch the cap -> cooldown -> escalation sequence happen live.
#ifdef TESTING_FAST_TIMERS
  #define FLOW_RATE_LPM        2.0f
  #define WATER_LIMIT_LOWLAND  0.33f  // caps after ~10s at 2.0 L/min
  #define WATER_LIMIT_HIGHLAND 0.20f  // caps after ~6s
#else
  #define FLOW_RATE_LPM        2.0f  // ⚠️ placeholder — verify against real emitter flow rate (L/min)
  #define WATER_LIMIT_LOWLAND  3.0f  // ⚠️ placeholder — liters per session, needs field test
  #define WATER_LIMIT_HIGHLAND 2.0f  // ⚠️ placeholder — liters per session, needs field test
#endif

struct WaterBudget {
  float         litersDispensed = 0.0f;
  unsigned long sessionStartMs  = 0;
};

// Resets its accumulator on justOpened; returns false immediately if the
// solenoid isn't open (matches the behavior the irrigation doc's
// evaluateZone() assumes when verifying the Q2 fix). Otherwise accumulates
// volume = flow_rate * elapsed_time and compares against the session limit.
inline bool waterLimitExceeded(WaterBudget& budget, float limitLiters,
                                bool solenoidOpen, bool justOpened) {
  if (justOpened) {
    budget.litersDispensed = 0.0f;
    budget.sessionStartMs  = millis();
  }

  if (!solenoidOpen) return false;

  unsigned long elapsedMs = millis() - budget.sessionStartMs;
  budget.litersDispensed  = FLOW_RATE_LPM * (elapsedMs / 60000.0f);

  return budget.litersDispensed >= limitLiters;
}