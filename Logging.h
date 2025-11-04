#pragma once
#include "Shared.h"
#include <FS.h>

bool   logFileOpen();
void   startLogging();
void   stopLogging();
String currentLogName();

// legacy row writer (still supported)
void   logWriteRow(const Sample& s);

// NEW: 1 Hz writer — pass all channels for a single second
void   logWriteRow1Hz(
  uint64_t unix_ms, uint32_t time_ms,
  float dp_Pa, float Va_mps,
  float tempP_C, float tempEnv_C,
  float absP_Pa, float RH_pct,
  float rho_kgm3            // <— added
);

// file utilities
void   listFilesJSON(class WebServer& server);