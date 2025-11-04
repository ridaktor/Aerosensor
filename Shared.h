#pragma once
#include <Arduino.h>
#include <Preferences.h>

class WebServer;
void setupHTTP(WebServer& server, void (*saveSettings)());

struct Sample { uint32_t t_ms; float dp_Pa, temp_C, Va_mps; };

// Globals (defined in .ino)
extern Preferences prefs;
extern Sample   lastS;
extern float    dp_zero;
extern float    rho;
extern bool     invertDP;
extern uint32_t logEveryMs;
extern uint32_t bootCounter;
extern bool     loggingOn;

extern bool     autoRho;     // always compute ρ from env sensor
extern float    envP_Pa;     // Pa
extern float    envT_C;      // °C
extern float    envRH;       // %
extern bool     envHasHum;   // BME280=true, BMP280=false

extern bool     g_showSpeed; // UI/log gating flag (true = show)

// Settings persistence hook from .ino
void setupHTTP(class WebServer& server, void (*saveSettings)());