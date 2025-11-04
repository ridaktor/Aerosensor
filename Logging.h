#pragma once
#include "Shared.h"
#include <FS.h>

// open/close and write CSV rows
bool  logFileOpen();
void  startLogging();
void  stopLogging();
String currentLogName();
void  logWriteRow(const Sample& s);

// file utilities
void  listFilesJSON(class WebServer& server);