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
  logFile = SPIFFS.open(curName, FILE_WRITE);
  if (!logFile) {
    // fallback single file name (metadata exhaustion safety)
    curName = "/current.csv";
    logFile = SPIFFS.open(curName, FILE_WRITE);
  }
  if (!logFile) { curName=""; loggingOn=false; return; }

  logFile.println("time_ms,dp_Pa,Va_mps,temp_C");
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

void logWriteRow(const Sample& s){
  if (!logFile) return;
  logFile.printf("%lu,%.4f,%.4f,%.3f\n",
                 (unsigned long)s.t_ms, s.dp_Pa, s.Va_mps, s.temp_C);
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