#include "Logging.h"
#include "Config.h"
#include "Shared.h"
#include <SPIFFS.h>
#include <WebServer.h>

static File   logFile;
static String curName;

static String makeNewLogName(){
  char buf[48];
  snprintf(buf, sizeof(buf), "/log_%lu_%lu.csv",
           (unsigned long)bootCounter, (unsigned long)millis());
  return String(buf);
}

bool logFileOpen(){ return (bool)logFile; }

void startLogging(){
  if (loggingOn) return;

  curName = makeNewLogName();

  // Create/truncate a NEW file so header is first
  logFile = SPIFFS.open(curName, FILE_WRITE);
  if (!logFile) {
    // fallback single filename; also truncate so header is first
    curName = "/current.csv";
    logFile = SPIFFS.open(curName, FILE_WRITE);
  }
  if (!logFile) { curName=""; loggingOn=false; return; }

  // Always write header for a new file
  logFile.println("unix_ms,time_ms,dp_Pa,Va_mps,tempP_C,tempEnv_C,absP_Pa,RH_pct,rho_kgm3");
  logFile.flush();

  loggingOn = true;
  Serial.printf("[logging] started: %s\n", curName.c_str());
}

void stopLogging(){
  if (!loggingOn) return;
  loggingOn = false;
  if (logFile) { logFile.flush(); logFile.close(); }
  Serial.println("[logging] stopped");
}

String currentLogName(){ return loggingOn ? curName : String(""); }

// Backward-compatible writer (kept for any legacy call sites)
void logWriteRow(const Sample& s){
  if (!logFile) return;
  const uint64_t unix_ms = (uint64_t)g_timeOffsetMs + (uint64_t)s.t_ms;
  // Use current global rho for the trailing column
  logFile.printf("%llu,%lu,%.4f,%.4f,%.3f,%.3f,%.1f,%.1f,%.4f\n",
                 (unsigned long long)unix_ms,
                 (unsigned long)s.t_ms,
                 s.dp_Pa, s.Va_mps,
                 s.tempP_C, s.tempEnv_C,
                 s.absP_Pa, s.RH_pct,
                 rho);
  logFile.flush();
}

// 1 Hz writer
void logWriteRow1Hz(
  uint64_t unix_ms, uint32_t time_ms,
  float dp_Pa, float Va_mps,
  float tempP_C, float tempEnv_C,
  float absP_Pa, float RH_pct,
  float rho_kgm3
){
  if (!logFile) return;
  logFile.printf("%llu,%lu,%.4f,%.4f,%.3f,%.3f,%.1f,%.1f,%.4f\n",
                 (unsigned long long)unix_ms,
                 (unsigned long)time_ms,
                 dp_Pa, Va_mps,
                 tempP_C, tempEnv_C,
                 absP_Pa, RH_pct,
                 rho_kgm3);
  logFile.flush();
}

void listFilesJSON(WebServer& server){
  String j = "{\"files\":[";
  bool first = true;

  File root = SPIFFS.open("/");
  File f = root.openNextFile();
  while (f) {
    if (!first) j += ",";
    j += "{\"name\":\""; j += f.name(); j += "\",\"size\":";
    j += String((unsigned long)f.size()); j += "}";
    first = false;
    f = root.openNextFile();
  }
  j += "]}";
  server.send(200, "application/json", j);
}