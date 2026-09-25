// vpd.h — Vapor Pressure Deficit calculation.
//
// ⚠️ STILL OPEN per the finalized irrigation doc, §5: "computeVPD() exact
// Tetens/Magnus implementation — needs writing and verifying against
// reference values." This is a standard Magnus-Tetens approximation,
// provided as a working placeholder so the firmware compiles and the
// VPD_START_GATE logic is exercisable — treat the output as unverified
// until checked against reference psychrometric tables.
#pragma once
#include <math.h>

// Returns VPD in kPa given air temperature (deg C) and relative humidity (%).
inline float computeVPD(float tempC, float relHumidityPct) {
  float svp = 0.6108f * expf((17.27f * tempC) / (tempC + 237.3f)); // saturation vapor pressure, kPa
  float avp = svp * (relHumidityPct / 100.0f);                     // actual vapor pressure, kPa
  return svp - avp;
}