#include "Config.h"
#include "Shared.h"
#include "SensorMS5525.h"
#include "Logging.h"
#include "WebUI.h"
#include "EnvSensor.h"
#include <algorithm>   // nth_element
#include <vector>      // std::vector
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <math.h>

WebServer server(80);

// ====== globals in Shared.h (defined here) ======
Preferences prefs;
Sample  lastS      = {0,0,0,0};
float   dp_zero    = 0.0f;
float   rho        = DEFAULT_RHO;
bool    invertDP   = false;
uint32_t logEveryMs= DEFAULT_LOG_MS;   // make sure DEFAULT_LOG_MS = 1000 in Config.h
uint32_t bootCounter = 0;
bool    loggingOn  = false;
bool g_showSpeed = true;   // default visible at boot
bool  autoRho   = true;   // always compute ρ from env sensor
float envP_Pa   = NAN;
float envT_C    = NAN;
float envRH     = NAN;
bool  envHasHum = false;
long long g_timeOffsetMs = 0;
int       g_tzOffsetMin  = 0;

// static uint32_t nextLogAt = 0;

// ---- Drift control & display gating (tune to taste) ----
// constexpr float    QUIET_DP_THRESH_PA = 1.0f;     // |dp| below → "quiet"
// constexpr uint32_t QUIET_TIME_MS      = 10000;    // quiet this long → nudge zero
// constexpr float    ZERO_NUDGE_ALPHA   = 0.02f;    // move 2% towards P per nudge

constexpr float    DISP_DP_THRESH_PA  = 2.0f;     // need |dp| ≥ this to show speed
constexpr uint32_t DISP_HOLD_MS       = 1000;     // sustain threshold this long
constexpr float    DP_DEADBAND        = 2.0f;     // Pa: suppress tiny bumps at rest

// Runtime state
// static uint32_t g_quietStartMs = 0;     // when we entered quiet
// static uint32_t g_overStartMs  = 0;     // when we exceeded display threshold

// ---- settings I/O ----
static void loadSettings() {
  dp_zero    = prefs.getFloat("dp_zero", 0.0f);
  invertDP   = prefs.getBool ("inv", false);
  logEveryMs = prefs.getUInt ("logms", DEFAULT_LOG_MS);
  autoRho    = true; // force auto ρ from env sensor
}
static void saveSettings() {
  prefs.putFloat("dp_zero", dp_zero);
  prefs.putBool ("inv", invertDP);
  prefs.putUInt ("logms", logEveryMs);
}

void setup() {
  Serial.begin(115200);
  delay(30);

  // Filesystem
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

  // NVS
  prefs.begin("aerosens", false);
  bootCounter = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCounter);
  loadSettings();

  // Sensors
  sensorBegin();
  if (fabsf(dp_zero) < 0.001f) {  // first boot
    doZero(2000);
    saveSettings();
  }
  envBegin();   // start BME/BMP280 (or BMP280)

  // Wi-Fi AP + mDNS
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASS);
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  Serial.printf("AP: %s  PW: %s  → http://%s/  or  http://%s.local/\n",
                AP_SSID, AP_PASS, AP_IP.toString().c_str(), MDNS_NAME);

  // Web server
  setupHTTP(server, saveSettings);
}

void loop() {
  // Refresh environment
  float pPa, tC_env, rH; 
  bool hasH;
  if (envRead(pPa, tC_env, rH, hasH)) {
    envP_Pa = pPa; 
    envT_C  = tC_env; 
    envRH   = rH; 
    envHasHum = hasH;
    if (autoRho) {
      float rhoNew = envComputeRho(envP_Pa, envT_C, envRH, envHasHum);
      if (rhoNew > 0.5f && rhoNew < 2.0f) rho = rhoNew;
    }
  }

  // ---- 1 Hz aggregator state (static so it persists) ----
  struct Agg {
    uint32_t  sec_idx = 0;     // epoch seconds key
    bool      init    = false;
    // We use small vectors—~20 samples/sec is tiny for ESP32 RAM
    std::vector<float> v_dp;
    float sum_tempP   = 0, sum_tempEnv = 0, sum_absP = 0, sum_RH = 0;
    uint16_t n        = 0;
    void reset(uint32_t sec) {
      sec_idx = sec; init = true;
      v_dp.clear(); v_dp.reserve(64);
      sum_tempP = sum_tempEnv = sum_absP = sum_RH = 0;
      n = 0;
    }
  };
  static Agg agg;

  // Read differential pressure & compute speed (with your nudge + gating)
  float P, T_pressure;  // T from MS5525 (°C)
  if (sensorReadPT(P, T_pressure)) {
    float dp_raw = (invertDP ? -1.0f : 1.0f) * (P - dp_zero);

    // Quiet auto-zero nudge
    static uint32_t g_quietStartMs = 0;
    constexpr float    QUIET_DP_THRESH_PA = 1.0f;
    constexpr uint32_t QUIET_TIME_MS      = 10000;
    constexpr float    ZERO_NUDGE_ALPHA   = 0.02f;

    if (fabsf(dp_raw) < QUIET_DP_THRESH_PA) {
      if (g_quietStartMs == 0) g_quietStartMs = millis();
      if (millis() - g_quietStartMs >= QUIET_TIME_MS) {
        dp_zero += (P - dp_zero) * ZERO_NUDGE_ALPHA;
        g_quietStartMs = millis();
      }
    } else {
      g_quietStartMs = 0;
    }

    float dp = (invertDP ? -1.0f : 1.0f) * (P - dp_zero);

    // Gating (require |dp| >= 2 Pa sustained for 1 s)
    constexpr float DP_GATE = 2.0f;
    constexpr uint32_t MIN_HOLD = 1000;
    static uint32_t gateStart = 0;
    static bool armed = false;
    if (fabsf(dp) >= DP_GATE) {
      if (!armed) { gateStart = millis(); armed = true; }
      if (millis() - gateStart >= MIN_HOLD) g_showSpeed = true;
    } else {
      g_showSpeed = false;
      armed = false;
    }

    // Deadband for tiny bumps
    constexpr float DP_DEADBAND = 2.0f;
    const float dp_used = (fabsf(dp) < DP_DEADBAND) ? 0.0f : dp;

    // Speed from ΔP (use magnitude, gating handled by g_showSpeed)
    const float rho_use = (rho > 0.01f) ? rho : 1.225f;
    const float Va_now = g_showSpeed ? sqrtf(2.0f * fabsf(dp_used) / rho_use) : 0.0f;

    // Publish latest sample for UI
    lastS.t_ms     = millis();
    lastS.dp_Pa    = dp;
    lastS.temp_C   = (envHasHum || !isnan(envT_C)) ? envT_C : T_pressure;
    lastS.Va_mps   = Va_now;
    lastS.tempP_C  = T_pressure;
    lastS.tempEnv_C= (envHasHum || !isnan(envT_C)) ? envT_C : NAN;
    lastS.absP_Pa  = isnan(envP_Pa) ? 0.0f : envP_Pa;
    lastS.RH_pct   = (envHasHum && !isnan(envRH)) ? envRH : 0.0f;

    // ---- 1 Hz binning by real time ----
    const uint64_t now_unix_ms = (uint64_t)g_timeOffsetMs + (uint64_t)millis();
    const uint32_t now_sec     = (uint32_t)(now_unix_ms / 1000ULL);

    if (!agg.init) {
      agg.reset(now_sec);
    }

    if (now_sec != agg.sec_idx) {
      // CLOSE previous bin → compute robust stats and write 1 Hz row
      if (agg.n > 0) {
        // Median ΔP
        auto v = agg.v_dp;
        std::nth_element(v.begin(), v.begin() + v.size()/2, v.end());
        float dp_median = v[v.size()/2];

        // Use median ΔP for speed
        float Va_1Hz = (fabsf(dp_median) < DP_DEADBAND || !g_showSpeed) ? 0.0f
                         : sqrtf(2.0f * fabsf(dp_median) / rho_use);

        // Means for others
        const float tempP   = agg.sum_tempP   / agg.n;
        const float tempEnv = (agg.sum_tempEnv > 0 && agg.n>0) ? (agg.sum_tempEnv/agg.n) : NAN;
        const float absP    = agg.sum_absP    / agg.n;
        const float RH      = (agg.sum_RH>0) ? (agg.sum_RH/agg.n) : 0.0f;

        // Timestamp for the row: align to bin end (exact second boundary)
        const uint64_t row_unix_ms = (uint64_t)agg.sec_idx * 1000ULL;   // end of that second
        const uint32_t row_time_ms = (uint32_t)((agg.sec_idx * 1000ULL) - (uint64_t)g_timeOffsetMs);

        if (loggingOn && logFileOpen()) {
                  logWriteRow1Hz(row_unix_ms, row_time_ms,
                 dp_median, Va_1Hz,
                 tempP, tempEnv,
                 absP, RH,
                 rho /* rho_kgm3 */);
        }
      }
      // OPEN new bin
      agg.reset(now_sec);
    }

    // accumulate current sample to active bin
    agg.v_dp.push_back(dp);
    agg.sum_tempP   += T_pressure;
    agg.sum_tempEnv += (envHasHum || !isnan(envT_C)) ? envT_C : 0.0f;
    agg.sum_absP    += isnan(envP_Pa) ? 0.0f : envP_Pa;
    agg.sum_RH      += (envHasHum && !isnan(envRH)) ? envRH : 0.0f;
    agg.n++;
  }

  // Service HTTP
  server.handleClient();
  delay(10); // keep loop lively for better stats; logging is 1 Hz by binning
}
