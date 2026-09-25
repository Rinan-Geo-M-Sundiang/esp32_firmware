// config_types.h — the value types fetched from RTDB, their fetch
// functions (Arduino-only, need RtdbClient/WiFi), and the PURE
// trigger-decision functions each Pump uses (portable — no Arduino
// dependency, so they compile in a native unit test build).
#pragma once
#include <time.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include "rtdb_client.h"
  #include "json_field.h"
#endif

// ─── Thresholds (legacy single-zone model — superseded by the dual-zone
// irrigation_types.h/irrigation_zones.h logic. Kept for reference only,
// unused by main.ino as of this revision.) ──────────────────────────────
struct Thresholds {
  float tempOn;
  float tempOff;
  float moistureOn;
  float moistureOff;
  float moistureWarn;
  float temperatureWarn;
  float humidityWarn;
  float tdsOn;
  float tdsOff;
};

#ifdef ARDUINO
inline Thresholds fetchRemoteThresholds(RtdbClient& client) {
  Thresholds defaults = {40.0f, 15.0f, 30.0f, 60.0f, 40.0f, 20.0f, 50.0f, 500.0f, 700.0f};

  String payload;
  if (!client.get("config/thresholds", payload)) return defaults;

  Serial.printf("RAW_JSON: %s\n", payload.c_str());

  Thresholds result = defaults;
  result.tempOn          = JsonField::asFloat(payload, "tempOn",          defaults.tempOn);
  result.tempOff         = JsonField::asFloat(payload, "tempOff",         defaults.tempOff);
  result.moistureOn      = JsonField::asFloat(payload, "moistureOn",      defaults.moistureOn);
  result.moistureOff     = JsonField::asFloat(payload, "moistureOff",     defaults.moistureOff);
  result.moistureWarn    = JsonField::asFloat(payload, "moistureWarn",    defaults.moistureWarn);
  result.temperatureWarn = JsonField::asFloat(payload, "temperatureWarn", defaults.temperatureWarn);
  result.humidityWarn    = JsonField::asFloat(payload, "humidityWarn",    defaults.humidityWarn);
  result.tdsOn           = JsonField::asFloat(payload, "tdsOn",           defaults.tdsOn);
  result.tdsOff          = JsonField::asFloat(payload, "tdsOff",          defaults.tdsOff);

  return result;
}
#endif

// Pure — legacy single-zone rule, kept for reference only, unused now.
inline bool decideIrrigation(float avgTemp, float avgMoisture,
                              const Thresholds& t, bool wasOn) {
  if (!wasOn) {
    return avgTemp >= t.tempOn || avgMoisture <= t.moistureOn;
  }
  return !(avgMoisture >= t.moistureOff);
}

// ─── Fertilizer schedule (single pump, shared window) ──────────────────────
struct FertSchedule {
  int  startHour;
  int  startMinute;
  int  endHour;
  int  endMinute;
  bool days[7]; // tm_wday order: Sun..Sat
};

#ifdef ARDUINO
inline FertSchedule fetchFertSchedule(RtdbClient& client) {
  FertSchedule defaults = { 6, 0, 6, 15, { false, true, false, true, false, true, false } };

  String payload;
  if (!client.get("config/fert_schedule", payload)) return defaults;
  if (payload == "null" || payload.length() == 0) return defaults;

  Serial.printf("FERT_SCHED_RAW: %s\n", payload.c_str());

  FertSchedule result = defaults;
  result.startHour   = JsonField::asInt(payload, "startHour",   defaults.startHour);
  result.startMinute = JsonField::asInt(payload, "startMinute", defaults.startMinute);
  result.endHour     = JsonField::asInt(payload, "endHour",     defaults.endHour);
  result.endMinute   = JsonField::asInt(payload, "endMinute",   defaults.endMinute);

  result.days[0] = JsonField::asBool(payload, "sun", defaults.days[0]);
  result.days[1] = JsonField::asBool(payload, "mon", defaults.days[1]);
  result.days[2] = JsonField::asBool(payload, "tue", defaults.days[2]);
  result.days[3] = JsonField::asBool(payload, "wed", defaults.days[3]);
  result.days[4] = JsonField::asBool(payload, "thu", defaults.days[4]);
  result.days[5] = JsonField::asBool(payload, "fri", defaults.days[5]);
  result.days[6] = JsonField::asBool(payload, "sat", defaults.days[6]);

  return result;
}

// Legacy — two-source version, unused now that there's a single fertilizer
// pump. Needs RtdbClient-fetched activeSource, so it's Arduino-only.
inline int fetchFertActiveSource(RtdbClient& client) {
  int defaultSource = 2;

  String payload;
  if (!client.get("config/fert_active_source", payload)) return defaultSource;

  if (payload.indexOf("organic")  >= 0) return 3;
  if (payload.indexOf("leachate") >= 0) return 2;
  return defaultSource;
}
#endif

// Pure — takes `now` as a parameter instead of calling time() itself, so
// it can be driven with a fixed clock in an off-device unit test. No
// Arduino dependency: time_t/localtime_r are standard C, available on
// your laptop's compiler too.
inline bool isFertScheduleActiveNow(const FertSchedule& s, time_t now) {
  if (now < 1000000000L) return false; // NTP not synced yet — fail-safe: no fire

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  if (!s.days[timeinfo.tm_wday]) return false;

  int nowMinutes   = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMinutes = s.startHour * 60 + s.startMinute;
  int endMinutes   = s.endHour   * 60 + s.endMinute;

  if (startMinutes <= endMinutes) {
    return nowMinutes >= startMinutes && nowMinutes < endMinutes;
  }
  return nowMinutes >= startMinutes || nowMinutes < endMinutes; // crosses midnight
}

// Production convenience — reads the real clock, delegates to the pure
// form. Portable (time() is standard C), but deliberately NOT what tests
// call — tests always pass an explicit `now` so they stay deterministic.
inline bool isFertScheduleActiveNow(const FertSchedule& s) {
  time_t now;
  time(&now);
  return isFertScheduleActiveNow(s, now);
}

// Pure — legacy two-source version, kept for reference only.
inline bool decideFertPump(const FertSchedule& s, time_t now,
                            int activeSource, int thisPumpSource) {
  return isFertScheduleActiveNow(s, now) && activeSource == thisPumpSource;
}

// Pure — single-pump version actually used by main.ino now. Fires
// whenever the shared schedule window is open.
inline bool decideFertilizer(const FertSchedule& s, time_t now) {
  return isFertScheduleActiveNow(s, now);
}