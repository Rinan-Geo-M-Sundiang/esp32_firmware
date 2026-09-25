// =============================================================================
//  offline_buffer.h — NVS-backed circular buffer for offline readings
//  Namespace : "offbuf"  |  Capacity : 50 readings
//  Each slot : key "r{idx}" → "temp,hum,ts"
//  Survives reboots. Oldest reading dropped silently when full.
// =============================================================================
#pragma once
#include <Preferences.h>

#define BUF_MAX 50
#define BUF_NS  "offbuf"

struct Reading {
  float         temp;
  float         hum;
  unsigned long ts;
};

class OfflineBuffer {
  Preferences _prefs;
  int _count = 0;
  int _head  = 0;
  int _tail  = 0;

  void _loadMeta() {
    if (!_prefs.begin(BUF_NS, true)) return;   // guard: NVS open failure
    _count = _prefs.getInt("cnt",  0);
    _head  = _prefs.getInt("head", 0);
    _tail  = _prefs.getInt("tail", 0);
    _prefs.end();
  }

  void _saveMeta() {
    if (!_prefs.begin(BUF_NS, false)) return;  // guard: NVS open failure
    _prefs.putInt("cnt",  _count);
    _prefs.putInt("head", _head);
    _prefs.putInt("tail", _tail);
    _prefs.end();
  }

public:
  void begin()        { _loadMeta(); }
  int  count() const  { return _count; }

  void push(float temp, float hum, unsigned long ts) {
    char key[6], val[48];
    snprintf(key, sizeof(key), "r%d", _tail);
    snprintf(val, sizeof(val), "%.2f,%.2f,%lu", temp, hum, ts);

    if (_prefs.begin(BUF_NS, false)) {
      _prefs.putString(key, val);
      _prefs.end();
    }

    _tail = (_tail + 1) % BUF_MAX;
    if (_count < BUF_MAX) _count++;
    else                  _head = (_head + 1) % BUF_MAX;
    _saveMeta();
  }

  bool pop(Reading &out) {
    if (_count == 0) return false;

    char key[6];
    snprintf(key, sizeof(key), "r%d", _head);

    String val;
    if (_prefs.begin(BUF_NS, false)) {
      val = _prefs.getString(key, "");
      _prefs.remove(key);
      _prefs.end();
    }

    _head = (_head + 1) % BUF_MAX;
    _count--;
    _saveMeta();

    if (val.isEmpty()) return false;

    sscanf(val.c_str(), "%f,%f,%lu", &out.temp, &out.hum, &out.ts);
    return true;
  }
};