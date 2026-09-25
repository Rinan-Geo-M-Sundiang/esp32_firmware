// json_field.h — shared, pure JSON field extraction.
//
// Guarden's RTDB payloads are small flat objects, so a hand-rolled string
// scan is fine (no library, no heap churn) — but the scan itself must live
// in exactly one place. Previously fetchRemoteThresholds() and
// fetchFertSchedule() each carried their own copy of this exact logic;
// a parsing edge case fixed in one silently stayed broken in the other.
//
// Pure functions: string in, value out. No WiFi, no hardware — testable
// off-device.
#pragma once
#include <Arduino.h>

namespace JsonField {

// Locates "key": <value> inside a flat JSON object and returns the raw
// (untyped) value substring, trimmed. Empty string if the key is missing
// or malformed.
inline String rawValue(const String& json, const char* key) {
  String searchKey = "\"";
  searchKey += key;
  searchKey += "\"";

  int pos = json.indexOf(searchKey);
  if (pos < 0) return String();

  pos = json.indexOf(":", pos);
  if (pos < 0) return String();
  pos++;

  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

  int endPos = pos;
  while (endPos < (int)json.length() && json[endPos] != ',' && json[endPos] != '}') endPos++;

  String val = json.substring(pos, endPos);
  val.trim();
  return val;
}

inline float asFloat(const String& json, const char* key, float def) {
  String raw = rawValue(json, key);
  if (raw.length() == 0) return def;

  bool looksNumeric = (raw[0] == '-' || raw[0] == '.' || (raw[0] >= '0' && raw[0] <= '9'));
  if (!looksNumeric) return def;

  return raw.toFloat();
}

inline int asInt(const String& json, const char* key, int def) {
  String raw = rawValue(json, key);
  if (raw.length() == 0) return def;
  return raw.toInt();
}

inline bool asBool(const String& json, const char* key, bool def) {
  String raw = rawValue(json, key);
  if (raw == "true")  return true;
  if (raw == "false") return false;
  return def;
}

} // namespace JsonField