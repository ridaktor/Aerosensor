#pragma once
#include <Arduino.h>
#include <Preferences.h>

class WebServer;

// Single forward declaration is enough:
void setupHTTP(WebServer& server, void (*saveSettings)());

struct Sample {
  uint32_t t_ms;
  float    dp_Pa;
  float    temp_C;     // env temperature preferred (kept for UI)
  float    Va_mps;
  // Extra channels for 1 Hz logging
  float    tempP_C;    // pressure-sensor temperature (MS5525)
  float    tempEnv_C;  // environment sensor temperature (BME/BMP)
  float    absP_Pa;    // environment absolute pressure (Pa)
  float    RH_pct;     // environment relative humidity (% or 0 if N/A)
};

// Globals (defined in .ino)
extern Preferences prefs;
extern Sample   lastS;
extern float    dp_zero;
extern float    rho;
extern bool     invertDP;
extern uint32_t logEveryMs;
extern uint32_t bootCounter;
extern bool     loggingOn;

extern bool     autoRho;
extern float    envP_Pa;
extern float    envT_C;
extern float    envRH;
extern bool     envHasHum;

extern bool     g_showSpeed;

extern long long g_timeOffsetMs; // epoch_ms - millis()
extern int       g_tzOffsetMin;  // minutes west of UTC