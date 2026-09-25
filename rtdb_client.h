// rtdb_client.h — the ONE place that talks HTTP to Firebase RTDB.
//
// Before: seven functions (three pushers, four fetchers) each rebuilt a
// WiFiClientSecure, formatted the same "<db>/<path>.json?auth=<secret>" URL,
// and repeated identical http.begin/GET-or-PUT/end + status handling.
// Now every call site is one line: client.get(path, out) / client.put(path, body).
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

class RtdbClient {
public:
  RtdbClient(const char* dbUrl, const char* secret)
    : _dbUrl(dbUrl), _secret(secret) {}

  // GET /<path>.json — fills `out` with the raw response body.
  // Returns false on no-WiFi, connect failure, or non-200.
  bool get(const char* path, String& out) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char url[192];
    snprintf(url, sizeof(url), "%s/%s.json?auth=%s", _dbUrl, path, _secret);

    if (!http.begin(client, url)) {
      Serial.printf("RTDB_ERR begin -> %s\n", path);
      return false;
    }

    int code = http.GET();
    out = http.getString();
    http.end();

    if (code != 200) {
      Serial.printf("RTDB_ERR GET %d -> %s\n", code, path);
      return false;
    }
    return true;
  }

  // PUT /<path>.json with a raw JSON body (object, number, or quoted string).
  bool put(const char* path, const char* jsonBody) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char url[192];
    snprintf(url, sizeof(url), "%s/%s.json?auth=%s", _dbUrl, path, _secret);

    if (!http.begin(client, url)) {
      Serial.printf("RTDB_ERR begin -> %s\n", path);
      return false;
    }

    http.addHeader(F("Content-Type"), F("application/json"));
    int code = http.PUT((uint8_t*)jsonBody, strlen(jsonBody));
    http.end();

    if (code < 200 || code > 299) {
      Serial.printf("RTDB_ERR PUT %d -> %s\n", code, path);
      return false;
    }
    return true;
  }

  // Convenience for the common "write one number" case (averages).
  bool putFloat(const char* path, float value) {
    char body[32];
    snprintf(body, sizeof(body), "%.2f", value);
    return put(path, body);
  }

  // Convenience for reading a boolean flag path (e.g. control/relay).
  bool getBool(const char* path, bool defaultVal = false) {
    String payload;
    if (!get(path, payload)) return defaultVal;
    return payload == "true";
  }

  // Convenience for reading a "*_updated" epoch-seconds timestamp path.
  // Returns 0 on failure or unset — callers treat 0 as "no signal yet".
  unsigned long getTimestamp(const char* path) {
    String payload;
    if (!get(path, payload)) return 0;
    return strtoul(payload.c_str(), nullptr, 10);
  }

private:
  const char* _dbUrl;
  const char* _secret;
};