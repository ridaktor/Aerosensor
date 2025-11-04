#pragma once
#include <Arduino.h>

// Initialize I2C and sensor (reset + PROM)
void sensorBegin();

// Read one pressure/temperature pair (Pa, °C). Returns true on success.
bool sensorReadPT(float &P_Pa, float &T_C);

// Zeroing helper: average ΔP with both ports open. Returns ok, writes dp_zero,
// optionally returns sample count via outSamples.
bool doZero(uint16_t ms = 2000, uint16_t* outSamples = nullptr);