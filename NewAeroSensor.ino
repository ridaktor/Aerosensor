#include "Config.h"
#include "Shared.h"
#include "SensorMS5525.h"
#include "Logging.h"
#include "WebUI.h"
#include "EnvSensor.h"

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include <math.h>

WebServer server(80);

// Config definitions (match externs in Config.h)
const char* AP_SSID   = "AeroSensor";
const char* AP_PASS   = "aero1234";
const char* MDNS_NAME = "aerosensor";
const IPAddress AP_IP  (192,168,4,1);
const IPAddress AP_GW  (192,168,4,1);
const IPAddress AP_MASK(255,255,255,0);

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

static uint32_t nextLogAt = 0;

// ---- Drift control & display gating (tune to taste) ----
constexpr float    QUIET_DP_THRESH_PA = 1.0f;     // |dp| below → "quiet"
constexpr uint32_t QUIET_TIME_MS      = 10000;    // quiet this long → nudge zero
constexpr float    ZERO_NUDGE_ALPHA   = 0.02f;    // move 2% towards P per nudge

constexpr float    DISP_DP_THRESH_PA  = 2.0f;     // need |dp| ≥ this to show speed
constexpr uint32_t DISP_HOLD_MS       = 1000;     // sustain threshold this long
constexpr float    DP_DEADBAND        = 2.0f;     // Pa: suppress tiny bumps at rest

// Runtime state
static uint32_t g_quietStartMs = 0;     // when we entered quiet
static uint32_t g_overStartMs  = 0;     // when we exceeded display threshold

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
  // 1) Refresh environment first (keeps rho current even if dp read fails)
  float pPa, tC, rH; 
  bool hasH;
  if (envRead(pPa, tC, rH, hasH)) {
    envP_Pa = pPa; 
    envT_C  = tC; 
    envRH   = rH; 
    envHasHum = hasH;
    if (autoRho) {
      float rhoNew = envComputeRho(envP_Pa, envT_C, envRH, envHasHum);
      if (rhoNew > 0.5f && rhoNew < 2.0f) rho = rhoNew;  // sanity clamp
    }
  }

  // 2) Read differential pressure & compute airspeed with zero-nudge + gating
  float P, T;
  if (sensorReadPT(P, T)) {
    // raw dp vs current zero
    float dp_raw = (invertDP ? -1.0f : 1.0f) * (P - dp_zero);

    // (A) Auto-nudge dp_zero when "quiet" long enough
    if (fabsf(dp_raw) < QUIET_DP_THRESH_PA) {
      if (g_quietStartMs == 0) g_quietStartMs = millis();
      if (millis() - g_quietStartMs >= QUIET_TIME_MS) {
        // move dp_zero gently towards P (independent of invertDP)
        dp_zero += (P - dp_zero) * ZERO_NUDGE_ALPHA;
        g_quietStartMs = millis(); // rate-limit nudges
      }
    } else {
      g_quietStartMs = 0;
    }

    // Recompute dp after possible nudge
    float dp = (invertDP ? -1.0f : 1.0f) * (P - dp_zero);

    // (B) Display/log gating
    if (fabsf(dp) >= DISP_DP_THRESH_PA) {
      if (g_overStartMs == 0) g_overStartMs = millis();
    } else {
      g_overStartMs = 0;
      g_showSpeed   = false;  // drop immediately when below threshold
    }
    if (!g_showSpeed && g_overStartMs != 0 && (millis() - g_overStartMs >= DISP_HOLD_MS)) {
      g_showSpeed = true;     // sustained above threshold
    }

    // Avoid divide-by-zero or negative rho
    float rho_use = (rho > 0.01f) ? rho : 1.225f;

    // Deadband to suppress tiny bumps at rest
    float dp_used = (fabsf(dp) < DP_DEADBAND) ? 0.0f : dp;

    // Compute displayed speed (gated)
    float Va = g_showSpeed ? sqrtf(2.0f * fabsf(dp_used) / rho_use) : 0.0f;
    // ---- after computing dp and Va ----

// gate: require |dp| >= DP_GATE for at least MIN_HOLD ms to show speed
constexpr float DP_GATE   = 2.0f;     // Pa (tune)
constexpr uint32_t MIN_HOLD = 1000;   // ms

static uint32_t gateStart = 0;
static bool armed = false;

if (fabsf(dp) >= DP_GATE) {
  if (!armed) { gateStart = millis(); armed = true; }
  if (millis() - gateStart >= MIN_HOLD) g_showSpeed = true;
} else {
  // fell below gate: hide immediately and disarm
  g_showSpeed = false;
  armed = false;
}

    // Publish latest sample
    lastS.t_ms   = millis();
    lastS.dp_Pa  = dp; // keep sign; speed uses magnitude
    lastS.temp_C = (envHasHum || !isnan(envT_C)) ? envT_C : T;
    lastS.Va_mps = Va;

    // Logging tick (logs gated speed)
    if (loggingOn && millis() >= nextLogAt && logFileOpen()) {
      nextLogAt = millis() + logEveryMs;
      logWriteRow(lastS);
    }
  }

  // Service HTTP
  server.handleClient();

  delay(50); // ~20 Hz
}