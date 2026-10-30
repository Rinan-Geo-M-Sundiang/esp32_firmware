// misting.h — highland misting logic.
//
// Rule (decision log item 3): misting starts when temperature >= tempOn
// OR relative humidity <= humidityOn, AND the water reservoir is safe.
// Low water stops misting, because the misting pump draws from the water
// reservoir (decision log item 4). Misting does not use VPD.
//
// Reuses: JsonField (parsing), ZoneReading (sensor input), Pump
// (relay + override handling), PolledConfig (called from main.ino).
// The decision is pure and has no Arduino dependency, so it is
// testable on a PC.
#pragma once
#include "json_field.h"
#include "irrigation_types.h"   // ZoneReading
#include "pump.h"

#ifdef ARDUINO
  #include "rtdb_client.h"
#endif

struct MistConfig {
  float tempOn;      // deg C — start at or above this
  float humidityOn;  // % RH  — start at or below this
};

// Defaults used until config/misting is read.
static const MistConfig MIST_DEFAULTS = { 32.0f, 70.0f };

// Pure — missing or bad fields fall back to `defaults`.
inline MistConfig parseMistConfig(const String& payload, const MistConfig& defaults) {
  MistConfig r = defaults;
  r.tempOn     = JsonField::asFloat(payload, "tempOn",     defaults.tempOn);
  r.humidityOn = JsonField::asFloat(payload, "humidityOn", defaults.humidityOn);
  return r;
}

#ifdef ARDUINO
inline MistConfig fetchMistConfig(RtdbClient& client, const char* path,
                                  const MistConfig& defaults) {
  String payload;
  if (!client.get(path, payload)) return defaults;
  Serial.printf("MIST_CFG_RAW %s: %s\n", path, payload.c_str());
  return parseMistConfig(payload, defaults);
}
#endif

class MistingController {
public:
  explicit MistingController(const MistConfig& cfg = MIST_DEFAULTS) : _cfg(cfg) {}

  void setConfig(const MistConfig& cfg) { _cfg = cfg; }
  const MistConfig& config() const { return _cfg; }

  // What auto mode wants right now. Fails closed on a bad sensor or
  // an unsafe water reservoir.
  bool decide(const ZoneReading& r, bool waterSafe) const {
    if (!waterSafe || !r.valid) return false;
    return r.temperature >= _cfg.tempOn || r.humidity <= _cfg.humidityOn;
  }

  // Decide, then drive the pump. Low water also cancels a manual
  // override (same rule as fertilizer lock). A sensor fault blocks
  // auto only; manual override still works.
  void update(Pump& pump, const ZoneReading& r, bool waterSafe) {
    if (!waterSafe) pump.overrideActive = false;
    pump.update(decide(r, waterSafe));
  }

private:
  MistConfig _cfg;
};
