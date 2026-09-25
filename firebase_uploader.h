// firebase_uploader.h — NTP, offline buffering, and zone-sensor/averages
// upload. HTTP mechanics now live in rtdb_client.h; this file only builds
// the JSON bodies and decides when to buffer vs. push.
#pragma once

#include <WiFi.h>
#include "rtdb_client.h"
#include "offline_buffer.h"

// ─── WiFi ───────────────────────────────────────────────────────────────────
#define WIFI_SSID    "HUAWEI-2.4G-7Sj5"
#define WIFI_PASS    "n2Abeu7Q"

// ─── Firebase RTDB ──────────────────────────────────────────────────────────
#define FIREBASE_DB_URL "https://esp32testing-477e7-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_SECRET "43yYixk5sz0ssIFjCFzVsvHaMdaKPyBX6UIko1ye"

// ─── NTP ────────────────────────────────────────────────────────────────────
#define NTP_SERVER     "pool.ntp.org"
#define NTP_GMT_OFFSET 28800L  // UTC+8 Philippines
#define NTP_DST_OFFSET 0L

#define NUM_ZONES 3

static OfflineBuffer _offBuf;
static bool          _ntpSynced = false;

inline void firebaseInitNTP() {
  if (_ntpSynced) return;
  configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
  _ntpSynced = true;
  Serial.println(F("NTP: sync requested"));
}

// Returns Unix epoch in SECONDS, or 0 if NTP hasn't synced yet.
inline unsigned long firebaseEpochSeconds() {
  time_t now;
  time(&now);
  return (now > 1000000000UL) ? (unsigned long)now : 0UL;
}

// Returns Unix epoch in MILLISECONDS, or 0 if NTP hasn't synced yet.
inline unsigned long long firebaseEpochMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec < 1000000000L) return 0ULL;
  return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

inline bool waitForNTP(unsigned long timeoutMs = 10000UL) {
  Serial.print("NTP syncing");
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    time_t now;
    time(&now);
    if (now > 1000000000UL) {
      Serial.printf(" OK  epoch=%lu\n", (unsigned long)now);
      return true;
    }
    delay(500);
    Serial.print('.');
  }
  Serial.println(" TIMEOUT — uploads will be skipped until sync");
  return false;
}

inline void uploaderBegin() {
  _offBuf.begin();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println(F("WIFI: connecting..."));
}

inline bool _pushZoneSensor(RtdbClient& client, int zone, float temp, float hum,
                             float moisture, unsigned long long tsMs) {
  moisture = constrain(moisture, 0.0f, 100.0f);

  char body[160];
  snprintf(body, sizeof(body),
           "{\"temperature\":%.2f,\"humidity\":%.2f,\"moisture\":%.2f,\"updatedAt\":%llu}",
           temp, hum, moisture, tsMs);

  char path[32];
  snprintf(path, sizeof(path), "sensors/zone-%d", zone);

  return client.put(path, body);
}

inline bool _pushAverages(RtdbClient& client, float avgTemp, float avgHum, float avgMoisture) {
  bool ok = true;
  if (!client.putFloat("averages/temperature", avgTemp)) ok = false;
  if (!client.putFloat("averages/humidity",    avgHum))  ok = false;
  if (!client.putFloat("averages/moisture",    avgMoisture)) ok = false;
  return ok;
}

inline bool _pushAll(RtdbClient& client, float avgTemp, float avgHum, float avgMoisture,
                      float temps[NUM_ZONES], float hums[NUM_ZONES], float moistures[NUM_ZONES],
                      bool validZones[NUM_ZONES], unsigned long long tsMs) {
  bool ok = true;

  for (int i = 0; i < NUM_ZONES; i++) {
    int zone = i + 1;
    if (!validZones[i]) {
      Serial.printf("ZONE_SKIP: zone-%d (sensor error)\n", zone);
      continue;
    }
    if (!_pushZoneSensor(client, zone, temps[i], hums[i], moistures[i], tsMs)) {
      Serial.printf("ZONE_ERR: zone-%d\n", zone);
      ok = false;
    } else {
      Serial.printf("ZONE_OK zone-%d: temp=%.2f hum=%.2f moist=%.2f\n",
                    zone, temps[i], hums[i], moistures[i]);
    }
  }

  if (!_pushAverages(client, avgTemp, avgHum, avgMoisture)) {
    Serial.println(F("AVG_ERR: failed to write averages"));
    ok = false;
  } else {
    Serial.printf("AVG_OK: temp=%.2f hum=%.2f moisture=%.2f tsMs=%llu\n",
                  avgTemp, avgHum, avgMoisture, tsMs);
  }

  return ok;
}

inline void uploadOrBuffer(RtdbClient& client, float avgTemp, float avgHum, float avgMoisture,
                            float temps[NUM_ZONES], float hums[NUM_ZONES],
                            float moistures[NUM_ZONES], bool validZones[NUM_ZONES]) {
  if (WiFi.status() != WL_CONNECTED) {
    _offBuf.push(avgTemp, avgHum, firebaseEpochSeconds());
    Serial.printf("OFFLINE: buffered (queue=%d)\n", _offBuf.count());
    return;
  }

  firebaseInitNTP();

  // Drain any readings buffered while offline.
  if (_offBuf.count() > 0) {
    Serial.printf("SYNC: draining %d buffered average(s)\n", _offBuf.count());
    Reading r;
    while (_offBuf.pop(r)) {
      if (!_pushAverages(client, r.temp, r.hum, 0.0f)) {
        _offBuf.push(r.temp, r.hum, r.ts);
        Serial.println(F("SYNC: REST error, aborting drain"));
        break;
      }
      delay(60);
    }
  }

  unsigned long long tsMs = firebaseEpochMs();
  if (tsMs == 0ULL) {
    Serial.println(F("UPLOAD_SKIP: NTP not yet synced, skipping this cycle"));
    return;
  }

  if (_pushAll(client, avgTemp, avgHum, avgMoisture, temps, hums, moistures, validZones, tsMs)) {
    Serial.printf("UPLOAD_OK tsMs=%llu\n", tsMs);
  } else {
    _offBuf.push(avgTemp, avgHum, (unsigned long)(tsMs / 1000ULL));
    Serial.println(F("UPLOAD_FAIL: averages buffered"));
  }
}
