// irrigation_zones.h — the SINGLE authoritative evaluateZone() and its
// call site, updateIrrigation(). Copied verbatim from
// "e-Tanim — ESP32 Irrigation Logic (FINALIZED v1.0)" — do not create a
// second evaluateZone() anywhere else in this codebase (Q1).
#pragma once
#include <Arduino.h>
#include "irrigation_types.h"
#include "water_budget.h"

inline void logIfChanged(bool value, bool& lastLogged, const char* label) {
  if (value != lastLogged) {
    Serial.printf("%s %s\n", label, value ? "ON" : "OFF");
    lastLogged = value;
  }
}

void evaluateZone(ZoneState& state, const ZoneReading& reading,
                   const ZoneThresholds& t, float vpdKpa,
                   IrrigationCause& latchedCause,
                   WaterBudget& budget, float waterLimit,
                   EmergencyCapHistory& capHistory,
                   unsigned long& normalCooldownUntilMs) {

  unsigned long now = millis();

  // ── Fail-safe first: dead sensor fails CLOSED, overrides manual too ──
  if (!reading.valid) {
    latchedCause      = NONE;
    state.autoActive  = false;
    state.finalActive = false;
    state.sensorFault = true;
    return;
  }
  state.sensorFault = false;

  // ── Mechanical lock — checked before anything else, blocks manual too.
  // No auto-expiry; only a dashboard clear-fault command resets this
  // (handled outside this function).
  if (capHistory.mechanicalLock) {
    latchedCause      = NONE;
    state.autoActive  = false;
    state.finalActive = false;   // manual override intentionally NOT consulted
    return;
  }

  float sm = reading.moisture;  // %FC

  if (sm >= t.smStop) {
    // Stop always wins for AUTO state, regardless of latch, VPD, or cooldown.
    latchedCause      = NONE;
    state.autoActive  = false;
  }
  else if (sm <= t.smEmergency && now < capHistory.cooldownUntilMs) {
    // Bug fix (self-grilled correction): emergency-dry AND still cooling
    // down from a recent capped emergency session. This must block
    // re-latching via EITHER the emergency branch OR the normal branch —
    // smEmergency is always drier than smNormal by definition, so any sm
    // this low automatically satisfies the normal branch's condition too.
    // Without this explicit block placed BEFORE the branch chain, a
    // cooldown-blocked emergency latch simply fell through to the normal
    // branch below and re-triggered under a different label — silently
    // defeating the cooldown, and worse, bypassing the escalation/lock
    // protection entirely, since escalation only counts caps tagged
    // EMERGENCY at the moment they're capped.
    if (latchedCause == NONE) {
      state.autoActive = false;   // block a FRESH re-latch via any path
    }
    // else: an already-active latched session holds its current state —
    // same "don't yank mid-flow" principle as the VPD-drop case below.
  }
  else if (sm <= t.smEmergency) {
    // Emergency is unconditional w.r.t. VPD — gated only by cooldown,
    // which is fully handled by the branch above.
    latchedCause      = EMERGENCY;
    state.autoActive  = true;
  }
  else if (latchedCause == NONE && sm <= t.smNormal) {
    // Q3 fix (found during hardware testing, 2nd pass): the VPD gate and
    // cooldown must gate a FRESH latch explicitly. The original version
    // folded both conditions into the elif itself with no else-branch —
    // when either failed, autoActive was left untouched, so a session
    // that had JUST been capped (autoActive still true from before the
    // cap) silently re-opened the solenoid one loop tick later, before
    // the cooldown had any chance to matter. Mirrors the explicit reset
    // already used in the emergency-cooldown branch above.
    if (vpdKpa >= VPD_START_GATE && now >= normalCooldownUntilMs) {
      latchedCause     = NORMAL;
      state.autoActive = true;
    } else {
      state.autoActive = false;
    }
  }
  // else: hold whatever is currently latched — the mid-session VPD-drop
  // case from the original design (VPD is a start gate only, never a
  // live stop condition).

  bool wasOpen       = state.finalActive;
  // Q4: manual override intentionally bypasses Stop AND cooldown — a
  // human explicitly requesting irrigation is a deliberate action, unlike
  // the mechanical-lock case where the system is protecting against water
  // going somewhere broken with nobody watching.
  state.finalActive  = state.manualOverride || state.autoActive;
  bool justOpened    = state.finalActive && !wasOpen;

  bool capped = waterLimitExceeded(budget, waterLimit, state.finalActive, justOpened);
  if (capped) {
    state.finalActive    = false;
    state.manualOverride = false;  // cap wins over override too — clear it
                                    // so the valve doesn't reopen next tick.

    if (latchedCause == EMERGENCY) {
      // ── Q2 fix: escalation memory (6-hr window) and routine cooldown
      // (30-min re-latch delay) are tracked independently. The cooldown
      // no longer zeroes the escalation timestamp.
      if (capHistory.escalationWindowStartMs != 0 &&
          (now - capHistory.escalationWindowStartMs) <= CAPPED_EMERGENCY_REPEAT_WINDOW_MS) {
        capHistory.mechanicalLock = true;   // 2nd capped event within 6 hr -> escalate
      } else {
        capHistory.escalationWindowStartMs = now;  // this is the 1st event in a fresh window
      }
      capHistory.cooldownUntilMs = now + CAPPED_EMERGENCY_ROUTINE_COOLDOWN_MS;
    }
    else if (latchedCause == NORMAL) {
      // Q3: lighter cooldown, no escalation tracking — normal caps are
      // expected/routine, this only exists to stop rapid re-open chatter.
      normalCooldownUntilMs = now + CAPPED_NORMAL_COOLDOWN_MS;
    }

    latchedCause = NONE;
  }

  // Escalation-window expiry: if 6 hours pass with no repeat, forget the
  // 1st event — fully decoupled from the 30-min cooldown (Q2 fix).
  if (!capHistory.mechanicalLock &&
      capHistory.escalationWindowStartMs != 0 &&
      (now - capHistory.escalationWindowStartMs) > CAPPED_EMERGENCY_REPEAT_WINDOW_MS) {
    capHistory.escalationWindowStartMs = 0;
  }
}

// ─── updateIrrigation() call site — declared as a function here; the
// actual global state (lowlandState, lowlandReading, pin numbers, etc.)
// lives in main.ino since it depends on your sensor-reading and override
// polling, which are wired per-project. ────────────────────────────────────
void updateIrrigation();