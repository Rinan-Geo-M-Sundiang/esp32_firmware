// pump.h — ONE pump module, instantiated three times.
//
// Before: pump 1 (updateRelay) and pumps 2/3 (updateFertPumps) each carried
// their own override flag, their own on/off state, and their own
// state-change logging as separate globals and separate functions. A fix to
// one pump's override/logging behavior did not fix the identical bug in the
// other two.
//
// Now: override-wins-over-auto, relay+solenoid actuation, and change
// logging live here once. Callers only ever pass in `autoDecision` — the
// result of a pure decide*() call from config_types.h — so Pump itself
// never has to know whether that decision came from a threshold or a
// schedule.
#pragma once
#include <Arduino.h>

struct Pump {
  const char* name;
  int  relayPin;
  int  solenoidPin;      // active-low, mirrors relayPin: LOW = open/on, HIGH = closed/off
  bool overrideActive = false;
  bool isOn           = false;

  // Explicit constructor — required because the private _lastLogged*
  // fields below disqualify Pump from being a plain aggregate, so
  // Pump p = { "name", pin, pin } (brace-init) no longer compiles without
  // this. Keeps the same call-site syntax you already have in main.ino.
  Pump(const char* n, int relay, int solenoid)
    : name(n), relayPin(relay), solenoidPin(solenoid) {}

  void begin() {
    pinMode(relayPin,    OUTPUT);
    pinMode(solenoidPin, OUTPUT);
    digitalWrite(relayPin,    HIGH); // closed on boot
    digitalWrite(solenoidPin, HIGH);
  }

  // `autoDecision` = what auto mode wants right now (from decideIrrigation()
  // or decideFertPump()). Override, when active, always wins.
  void update(bool autoDecision) {
    if (overrideActive != _lastLoggedOverride) {
      if (overrideActive) Serial.printf("%s ON  (dashboard override)\n", name);
      _lastLoggedOverride = overrideActive;
    }

    bool desired = overrideActive ? true : autoDecision;

    digitalWrite(relayPin,    desired ? LOW : HIGH);
    digitalWrite(solenoidPin, desired ? LOW : HIGH);

    if (desired != _lastLoggedOn) {
      Serial.printf("%s %s  (%s)\n", name, desired ? "ON" : "OFF",
                    overrideActive ? "override" : "auto");
      _lastLoggedOn = desired;
    }

    isOn = desired;
  }

private:
  bool _lastLoggedOn       = false;
  bool _lastLoggedOverride = false;
};