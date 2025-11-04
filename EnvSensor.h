#pragma once
#include <Arduino.h>

// Call at setup (tries 0x77 then 0x76). Works for BME280 and BMP280.
void envBegin();

// Returns true if a sensor was found
bool envAvailable();

// Read latest env values; returns true if fresh data
// p_Pa (Pa), t_C (°C), rh_pct (0..100), hasHumidity=true for BME280
bool envRead(float &p_Pa, float &t_C, float &rh_pct, bool &hasHumidity);

// Compute air density ρ (kg/m³). If hasHumidity=false, uses dry-air formula.
float envComputeRho(float p_Pa, float t_C, float rh_pct, bool hasHumidity);