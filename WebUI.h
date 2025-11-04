#pragma once
#include <Arduino.h>

class WebServer;

// Builds endpoints and serves UI. Call from setup().
void setupHTTP(WebServer& server, void (*saveSettingsCb)());