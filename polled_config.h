// polled_config.h — one generic "poll a *_updated timestamp, refetch the
// real value if it changed" implementation.
//
// Before: pollThresholdsIfNeeded(), pollFertScheduleIfNeeded(), and
// pollFertSourceIfNeeded() each hand-rolled this exact shape with their own
// interval/lastKnownTs/bootLoaded variables. Adding a fourth polled value
// meant copy-pasting a fourth function; now it's one line.
#pragma once
#include <functional>
#include "rtdb_client.h"

template <typename T>
class PolledConfig {
public:
  // fetchFn: pure "given a client, return the current remote value" fetcher.
  // std::function instead of a raw function pointer so a lambda can be
  // passed directly (e.g. capturing which RTDB path/defaults to use) —
  // no need to hand-write a differently-named wrapper function per value
  // just to match a fixed function-pointer signature.
  PolledConfig(RtdbClient& client, const char* updatedPath,
               unsigned long intervalMs, std::function<T(RtdbClient&)> fetchFn, T initial)
    : _client(client), _updatedPath(updatedPath), _intervalMs(intervalMs),
      _fetchFn(fetchFn), _value(initial) {}

  // Call every loop(). Refetches only when due AND the remote timestamp
  // changed. Returns true if the cached value was just refreshed.
  bool poll() {
    unsigned long now = millis();
    if (now - _lastCheck < _intervalMs) return false;
    _lastCheck = now;

    unsigned long remoteTs = _client.getTimestamp(_updatedPath);
    if (remoteTs == 0 && _bootLoaded) return false; // fetch failed, keep cached value

    if (!_bootLoaded || remoteTs != _lastKnownTs) {
      _value = _fetchFn(_client);
      if (remoteTs != 0) _lastKnownTs = remoteTs;
      _bootLoaded = true;
      return true;
    }
    return false;
  }

  // Unconditional fetch, used once at boot so the device doesn't run on
  // hardcoded defaults for the first `intervalMs` if WiFi is already up.
  void forceFetch() {
    _value = _fetchFn(_client);
    _lastKnownTs = _client.getTimestamp(_updatedPath);
    _bootLoaded = true;
  }

  const T& value() const { return _value; }

private:
  RtdbClient&   _client;
  const char*   _updatedPath;
  unsigned long _intervalMs;
  std::function<T(RtdbClient&)> _fetchFn;
  T _value;
  unsigned long _lastCheck   = 0;
  unsigned long _lastKnownTs = 0;
  bool          _bootLoaded  = false;
};