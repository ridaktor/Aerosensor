#include "EnvSensor.h"
#include "Config.h"
#include <math.h>

#if USE_ADAFRUIT_BME280
  #include <Adafruit_BME280.h>
  static Adafruit_BME280 bme;
  static bool g_has=false, g_hasHum=false;

  void envBegin() {
    if (bme.begin(ENV_ADDR_PRI)) { g_has = true; g_hasHum = (bme.sensorID() != 0x58); return; }
    if (bme.begin(ENV_ADDR_ALT)) { g_has = true; g_hasHum = (bme.sensorID() != 0x58); return; }
    g_has = false; g_hasHum = false;
  }
  bool envAvailable(){ return g_has; }

  bool envRead(float &p_Pa, float &t_C, float &rh_pct, bool &hasHumidity){
    if (!g_has) return false;
    t_C = bme.readTemperature();
    p_Pa = bme.readPressure();   // Pa
    if (isnan(t_C) || isnan(p_Pa)) return false;
    if (g_hasHum) { rh_pct = bme.readHumidity(); hasHumidity = !isnan(rh_pct); }
    else { rh_pct = 0.0f; hasHumidity = false; }
    return true;
  }
#else
  void envBegin() {}
  bool envAvailable(){ return false; }
  bool envRead(float &p_Pa, float &t_C, float &rh_pct, bool &hasHumidity){
    (void)p_Pa; (void)t_C; (void)rh_pct; (void)hasHumidity; return false;
  }
#endif

// Moist/dry density
float envComputeRho(float p_Pa, float t_C, float rh_pct, bool hasHumidity) {
  const float T = t_C + 273.15f;
  if (T <= 0.0f) return 1.225f;
  const float R_d = 287.05f;    // dry air
  const float R_v = 461.495f;   // water vapor
  if (hasHumidity) {
    // Magnus (over water) saturation vapor pressure (Pa)
    const float es_hPa = 6.112f * expf((17.62f * t_C) / (243.12f + t_C));
    const float es = es_hPa * 100.0f;
    const float pv = constrain(rh_pct, 0.0f, 100.0f)/100.0f * es; // Pa
    const float pd = max(0.0f, p_Pa - pv);                        // Pa
    return (pd/(R_d*T)) + (pv/(R_v*T));
  } else {
    return p_Pa / (R_d * T);
  }
}